import argparse
import posixpath
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
import zipfile
from decimal import Decimal, InvalidOperation, ROUND_HALF_UP
from pathlib import Path


MAIN_NS = "http://schemas.openxmlformats.org/spreadsheetml/2006/main"
REL_NS = "http://schemas.openxmlformats.org/officeDocument/2006/relationships"
PKG_REL_NS = "http://schemas.openxmlformats.org/package/2006/relationships"


def column_index(cell_reference):
    letters = re.match(r"[A-Z]+", cell_reference)
    if letters is None:
        raise ValueError(f"invalid spreadsheet cell reference: {cell_reference}")
    result = 0
    for character in letters.group(0):
        result = result * 26 + ord(character) - ord("A") + 1
    return result - 1


def worksheet_path(archive, sheet_name):
    workbook = ET.fromstring(archive.read("xl/workbook.xml"))
    relationships = ET.fromstring(
        archive.read("xl/_rels/workbook.xml.rels")
    )
    targets = {
        relationship.attrib["Id"]: relationship.attrib["Target"]
        for relationship in relationships.findall(
            f"{{{PKG_REL_NS}}}Relationship"
        )
    }
    for sheet in workbook.findall(f".//{{{MAIN_NS}}}sheet"):
        if sheet.attrib["name"] == sheet_name:
            target = targets[sheet.attrib[f"{{{REL_NS}}}id"]]
            if target.startswith("/"):
                return target.lstrip("/")
            return posixpath.normpath(posixpath.join("xl", target))
    raise ValueError(f"worksheet not found: {sheet_name}")


def shared_strings(archive):
    path = "xl/sharedStrings.xml"
    if path not in archive.namelist():
        return []
    root = ET.fromstring(archive.read(path))
    return [
        "".join(node.text or "" for node in item.findall(f".//{{{MAIN_NS}}}t"))
        for item in root.findall(f"{{{MAIN_NS}}}si")
    ]


def cell_value(cell, strings):
    kind = cell.attrib.get("t")
    if kind == "inlineStr":
        return "".join(
            node.text or "" for node in cell.findall(f".//{{{MAIN_NS}}}t")
        )
    value = cell.find(f"{{{MAIN_NS}}}v")
    if value is None or value.text is None:
        return ""
    if kind == "s":
        return strings[int(value.text)]
    return value.text


def workbook_rows(path, sheet_name):
    with zipfile.ZipFile(path) as archive:
        strings = shared_strings(archive)
        worksheet = ET.fromstring(archive.read(worksheet_path(archive, sheet_name)))
        for row in worksheet.findall(f".//{{{MAIN_NS}}}sheetData/{{{MAIN_NS}}}row"):
            cells = {}
            for cell in row.findall(f"{{{MAIN_NS}}}c"):
                cells[column_index(cell.attrib["r"])] = cell_value(cell, strings)
            yield cells


def read_student_rows(path, sheet_name):
    rows = iter(workbook_rows(path, sheet_name))
    try:
        headings = next(rows)
    except StopIteration as error:
        raise ValueError("the worksheet is empty") from error
    heading_index = {
        value.strip().casefold(): index for index, value in headings.items()
    }
    try:
        roll_index = heading_index["roll no"]
        name_index = heading_index["name"]
        cgpa_index = heading_index["cgpa"]
    except KeyError as error:
        raise ValueError(f"required workbook heading not found: {error.args[0]}") from error

    students = []
    for row_number, row in enumerate(rows, start=2):
        roll = row.get(roll_index, "").strip()
        name = row.get(name_index, "").strip()
        cgpa_text = row.get(cgpa_index, "").strip()
        if not roll and not name and not cgpa_text:
            continue
        digits = "".join(character for character in roll if character.isdigit())
        if len(digits) < 3 or not name or not cgpa_text:
            raise ValueError(f"incomplete roll number, name, or CGPA at row {row_number}")
        try:
            cgpa = Decimal(cgpa_text).quantize(
                Decimal("0.01"), rounding=ROUND_HALF_UP
            )
        except InvalidOperation as error:
            raise ValueError(f"invalid CGPA at row {row_number}: {cgpa_text}") from error
        if not Decimal("0") <= cgpa <= Decimal("10"):
            raise ValueError(f"CGPA out of range at row {row_number}: {cgpa_text}")
        students.append((digits[-3:], name, format(cgpa, ".2f")))
    if not students:
        raise ValueError("the worksheet contains no complete student rows")
    tags = [student[0] for student in students]
    if len(tags) != len(set(tags)):
        raise ValueError("the last three roll-number digits are not unique")
    return students


def quote(value):
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(
        description="Import RNO, Name, and CGPA from an .xlsx worksheet into MiniDB."
    )
    parser.add_argument("workbook", type=Path, help="source .xlsx workbook")
    parser.add_argument(
        "--sheet", default="Results", help="worksheet name (default: Results)"
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=root / "temp_data.db",
        help="new database path (default: temp_data.db in the project directory)",
    )
    parser.add_argument(
        "--executable",
        type=Path,
        default=root / ("minidb.exe" if sys.platform == "win32" else "minidb"),
        help="MiniDB executable path",
    )
    args = parser.parse_args()

    if not args.workbook.is_file():
        parser.error(f"workbook not found: {args.workbook}")
    if not args.executable.is_file():
        parser.error(f"MiniDB executable not found: {args.executable}; build it first")
    output = args.output.resolve()
    if output.exists() or Path(str(output) + ".lock").exists():
        parser.error(f"refusing to overwrite an existing database or lock: {output}")

    try:
        students = read_student_rows(args.workbook, args.sheet)
    except (OSError, KeyError, ValueError, zipfile.BadZipFile, ET.ParseError) as error:
        parser.error(str(error))

    commands = [
        "RENAME COLUMN tag RNO",
        "RENAME COLUMN data Name",
        "ADD COLUMN CGPA",
    ]
    commands.extend(
        f"INSERT {quote(rno)} {quote(name)} {quote(cgpa)}"
        for rno, name, cgpa in students
    )
    commands.append("EXIT")
    process = subprocess.run(
        [str(args.executable.resolve()), str(output)],
        input=("\n".join(commands) + "\n").encode("utf-8"),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if process.returncode != 0:
        sys.stderr.write(process.stderr.decode(errors="replace"))
        sys.stderr.write(process.stdout.decode(errors="replace"))
        raise SystemExit(process.returncode)
    print(f"Imported {len(students)} rows into {output}")
    print("Columns: RNO, Name, CGPA")
    print("RNO contains the final three digits of each roll number.")


if __name__ == "__main__":
    main()
