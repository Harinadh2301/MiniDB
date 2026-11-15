# MiniDB design decisions

This log records the implementation choices from the v1.1 design.

| Topic | Choice | Rationale |
|---|---|---|
| Index | In-memory B+ Tree, default order 4 | Separates routing from leaf record references and supports ordered scans. |
| Tree capacity | `BT_ORDER` internal children; `BT_ORDER - 1` leaf entries | Compile-time tuning exercises splits at small orders and supports larger benchmark orders. |
| Deletion | Rebalance by borrowing and merging | Maintains minimum occupancy and shrinks an empty root. |
| Default schema | Columns `tag` and `data` in `temp_data.db` | Starts generic rather than assuming a student table; both headings can be renamed. |
| Row key | Unique string tag in the first schema column | Supports generic identifiers such as padded roll-number suffixes. |
| Record reference | 32-bit slot number | Stable across process restarts without persisting process pointers or platform-sized offsets. |
| Record format | Fixed-size 8196-byte explicitly serialized row slots | Provides room for up to 32 dynamic text columns without shifting existing slots when a column is added. |
| Database header | 4096-byte versioned header with column names and row slots after it | Persists the schema alongside the data. |
| Index persistence | Rebuild from live records at startup | Keeps the record file as the source of truth in v1. |
| Values | Text cells, up to 255 bytes each; up to 32 columns | Allows datasets beyond student records and supports runtime column additions. |
| Tags | Printable unique strings, up to 63 bytes | Keeps a compact B+ Tree key while preserving leading zeros. |
| Locking | Exclusive `<database>.lock` file | Prevents a second local process from writing the same single-user database. |
| Durability | Flush and sync at clean close; no write-ahead log | A clean close is persisted, but mid-write crash recovery is not promised. |

Persistent index pages, freelist slot reuse, SQL, multiple tables, concurrent clients,
and write-ahead logging are intentionally deferred.
