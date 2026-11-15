# MiniDB

MiniDB is a small C11 single-user database with a B+ Tree index, a persistent
binary file, and a configurable set of columns. It is not limited to student
records.

## Build and test

```sh
gcc -std=c11 -Wall -Wextra -Wpedantic -g -Iinclude src/*.c -o minidb
make test
make bench
```

On Windows, use MinGW GCC and `mingw32-make`. The project has no third-party
runtime dependencies.

## Open a database

```sh
./minidb [database-file]
```

The default database is `temp_data.db`. A new database starts with two columns:
`tag` (the unique row key) and `data`. To use another database name, pass its
path. On Windows, run `.\minidb.exe [database-file]`.

When standard input is piped, prompts and the startup banner are suppressed.
Names and cell values containing spaces can be quoted, for example
`INSERT 001 "Ava Student"`; use `""` for an empty cell.

## Commands

```text
INSERT <tag> <value-for-column-2> ...
SELECT <tag>
SELECT RANGE <first-tag> <last-tag>
UPDATE <tag> <column> <value>
DELETE <tag>
DISPLAY [LIMIT <n>]
ADD COLUMN <name>
RENAME COLUMN <old-name> <new-name>
COLUMNS
HELP
EXIT
```

`INSERT` takes exactly one value for every column except `tag`, in the order
shown by `COLUMNS`. Tags are strings and are unique; sorting and range scans use
lexicographic tag order. `ADD COLUMN` appends a blank column to existing rows;
new rows must then provide a value for it. Rename the default columns with
`RENAME COLUMN tag <name>` and `RENAME COLUMN data <name>`. Up to 32 columns can
be stored, with values up to 255 bytes and tags up to 63 bytes.

Example:

```text
RENAME COLUMN tag RNO
RENAME COLUMN data Name
ADD COLUMN CGPA
INSERT 001 "Ava Student" 8.12
UPDATE 001 CGPA 8.25
DISPLAY
```

## Import the provided results workbook

The standard-library importer reads the `Results` worksheet and imports only
`Roll No`, `Name`, and `CGPA`. It saves the final three digits of each roll
number as `RNO`, preserving leading zeros, and names the columns `RNO`, `Name`,
and `CGPA`. The default output is `temp_data.db`; the importer refuses to
overwrite an existing database or lock.
The supplied workbook imports 189 records; the final RNO in that file is `192`
(some suffixes are absent from the source sheet).

```powershell
mingw32-make build
python tools\import_results.py C:\path\to\results_Full.xlsx
.\minidb.exe temp_data.db
```

Choose a different new output file with `--output`; the source workbook is not
modified. No spreadsheet packages need to be installed.

## Storage and index

The versioned 4096-byte header records the schema and database counters. Rows
are fixed-size, explicitly serialized binary slots; no C struct is written
directly. The B+ Tree indexes the string tag and is rebuilt from live rows on
open. Renamed or added columns are persisted in the header, and newly added
columns read as empty for existing rows.

An exclusive `<database>.lock` file prevents concurrent writers. If a process
crashes, verify no MiniDB process still uses the database before removing a
stale lock. A clean `EXIT` flushes and syncs data; MiniDB does not promise
recovery from a crash during a write and does not have a transaction log.

## Tests and limitations

`make test` runs parser, record-format, randomized string-key B+ Tree, schema
change, persistence, lock, corrupt-header, script-output, and overlong-input
tests. `make bench` compares indexed and linear lookups at 10k, 100k, and 1M
keys.

MiniDB remains a single-process educational database: it has no SQL, multiple
tables, concurrent clients, WAL, or index-page persistence. Deleted slots are
not reused. The binary format is specific to MiniDB.

See [docs/DECISIONS.md](docs/DECISIONS.md) for implementation choices.
