# mini_database

A small, educational single-table database written in C. It runs as an
interactive prompt, stores rows in a paged file on disk, and lets you choose the
columns when you create a database. It is inspired by SQLite but is not
compatible with SQLite files or SQL.

## Project background

> **Use the `generic_tables` branch for everything below.** The `main` branch is
> the fixed-schema version built while following
> [cstack's "Let's Build a Simple Database"](https://cstack.github.io/db_tutorial/).
> `generic_tables` adds user-defined schemas.

A Python B-tree prototype lives in [concepts/b_tree.py](concepts/b_tree.py). You
can run and study it on its own.

## Build

You need a C compiler (`gcc` or `clang`), `make`, and a POSIX system (Linux or
macOS).

```sh
git switch generic_tables
make
```

This creates `build/mini_database`.

## Demo

**1. Create a database.** The schema is given on the command line, only when the
file is first created:

```sh
./build/mini_database shop.db id:int name:text:20 price:int
```

You'll get a `db >` prompt. Steps 2-5 are typed at that prompt.

**2. Check the schema:**

```text
.schema
```

```text
Schema:
  - id : int (primary key)
  - name : text(20)
  - price : int
Row size: 29 bytes, 141 rows per page
```

**3. Add rows.** Wrap text in double quotes if it contains spaces:

```text
insert 1 "green tea" 450
insert 2 coffee 620
```

**4. Try a bad row.** This name is longer than the 20 bytes the column allows:

```text
insert 3 "a name that is far too long for this column" 100

String is too long.
```

**5. Read everything back, then quit:**

```text
select
```

Should print the following:

```text
(1, green tea, 450)
(2, coffee, 620)
Executed.
```

To quit

```text
.exit
```

**6. Reopen it.** Run this in your shell, **without** the schema (it is stored in
the file):

```sh
./build/mini_database shop.db
```

At the prompt, type `select` again. Your two rows are still there.

Starting a new file without a schema gives it a default one
(`id:int username:text:32 email:text:255`)

```sh
./build/mini_database people.db
```

## Commands

| Command | What it does |
| --- | --- |
| `insert v1 v2 ...` | Add a row: one value per column, in schema order |
| `select` | Print every row, in insertion order |
| `.schema` | Show the columns, row size, and rows per page |
| `.exit` | Save and quit (Ctrl-D does the same) |

Schema syntax is `name:int` or `name:text:MAX_BYTES`. The first column must be
an `int`. Try more columns of your own, or load a CSV: see
[Importing a CSV](docs/csv-import.md).

## Specifications

- One table per database file, with `int` and fixed-length `text` columns only.
- Only `insert` and `select`: no `update`, `delete`, or `where`.
- Duplicate ids are currently allowed. The first column is labeled "primary
  key", but uniqueness is not enforced yet.
- Capacity is limited: up to 10 columns, 512 bytes per row, and about 400 KiB
  per file.
- Data is written when you exit. If the process is killed first, that session's
  changes are lost.
