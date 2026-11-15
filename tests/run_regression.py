import os
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
EXECUTABLE = ROOT / ("minidb.exe" if os.name == "nt" else "minidb")


def run(database, commands):
    return subprocess.run(
        [str(EXECUTABLE), str(database)],
        input=commands,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


with tempfile.TemporaryDirectory(prefix="minidb-regression-") as temporary:
    directory = Path(temporary)
    script = (ROOT / "tests" / "regression.in").read_bytes()
    expected = (ROOT / "tests" / "regression.expected").read_bytes()
    expected = expected.replace(b"\r\n", b"\n")
    result = run(directory / "script.db", script)
    output = result.stdout.replace(b"\r\n", b"\n")
    if result.returncode != 0 or result.stderr or output != expected:
        raise SystemExit(
            "script regression failed\n"
            f"exit code: {result.returncode}\n"
            f"stderr: {result.stderr.decode(errors='replace')}\n"
            f"stdout:\n{output.decode(errors='replace')}"
        )

    overlong = b"X" * 17000 + b"\nINSERT 3 Name\nEXIT\n"
    result = run(directory / "longline.db", overlong)
    expected_longline = (
        b"Error: command line is too long.\n"
        b"1 record inserted.\n"
        b"Database closed.\n"
    )
    output = result.stdout.replace(b"\r\n", b"\n")
    if result.returncode != 0 or result.stderr or output != expected_longline:
        raise SystemExit(
            "overlong-line regression failed\n"
            f"exit code: {result.returncode}\n"
            f"stderr: {result.stderr.decode(errors='replace')}\n"
            f"stdout:\n{output.decode(errors='replace')}"
        )

print("Script-mode regression tests passed.")
