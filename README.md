# linkbrarian

**linkbrarian** is a small local-first command-line tool for saving links and
keeping a Markdown notes folder next to each one. It stores its catalogue in
SQLite under the user's home directory, independent of the current working
directory.

The design is deliberately minimal: one C99 translation unit, a small
`config.h`, a plain `Makefile`, SQLite for storage and libcurl for fetching page
metadata. There is no daemon, no browser, no JavaScript execution and no
network sync.

```sh
linkbrarian "https://example.com/article"
lbr         "https://example.com/article"

linkbrarian "https://example.com/article" note
lbr         "https://example.com/article" note
```

## Requirements

- A C99 compiler (`cc`, `gcc` or `clang`)
- `make`
- SQLite 3 development files
- libcurl development files
- `sqlite3` command-line tool for the test suite

Examples:

```sh
# Alpine
apk add build-base sqlite sqlite-dev curl-dev

# Debian / Ubuntu
apt install build-essential libsqlite3-dev libcurl4-openssl-dev sqlite3
```

## Build

```sh
make
```

This produces:

- `linkbrarian` - the main executable
- `lbr` - the equivalent short-name executable
- `tests/httpd` - the local fixture server used by the tests

Both command names run the same program and use the same database.

## Install

```sh
make install
```

The default prefix is `/usr/local`; override it as usual:

```sh
make install PREFIX="$HOME/.local"
```

Uninstall with:

```sh
make uninstall
```

## Usage

```text
linkbrarian [-hv] URL [note]
lbr         [-hv] URL [note]
```

- `URL` must be an absolute `http://` or `https://` URL.
- `note` is the only optional positional argument. It creates a notes folder
  and an initial `README.md`.
- Extra positional arguments are rejected.

Options:

```text
-h, --help     show help
-v, --version  show the version
```

Exit codes:

| Code | Meaning |
|---:|---|
| `0` | link saved, or the link was already registered |
| `1` | invalid command-line arguments |
| `2` | invalid URL |
| `3` | storage or database error |
| `4` | notes folder could not be created |

Normal output goes to `stdout`; errors go to `stderr`.

## Storage

Data lives in the user's home directory:

| System | Data directory | Database |
|---|---|---|
| Windows | `%USERPROFILE%\.linkbrarian\` | `%USERPROFILE%\.linkbrarian\linkography.db` |
| Unix-like | `$HOME/.linkbrarian/` | `$HOME/.linkbrarian/linkography.db` |

`.linkbrarian` is created automatically (mode `0700` on Unix-like systems).
The current working directory is never used for storage.

## Behaviour

### Title resolution

For a valid URL, `page_name` is resolved in this order:

1. The HTML `<title>` element.
2. The `og:title` metadata value.
3. The URL host.

The HTTP request follows redirects, uses a 5 second connect timeout, a 15 second
total timeout and accepts at most 1 MiB of response data. Non-HTML responses,
HTTP errors, connection failures and missing titles all fall back to the host.
Query strings and fragments are never removed from the stored URL.

HTML entities are decoded, tags inside the title are stripped and surrounding
Unicode whitespace is removed. Tag and attribute names are matched
case-insensitively.

### Identity and numbering

- `link` is unique. Running the command again for the same URL does not create
  another row; the existing record is reported.
- Different URLs with the same visible title are distinct records.
- `n` starts at `1` for the first record of a title and increments for each
  additional URL with the same title.
- Grouping uses `page_name_key`: Unicode whitespace is collapsed, surrounding
  whitespace is removed and case is folded for ASCII and Latin-1 characters.
  The visible `page_name` keeps its original casing and interior spacing.
- `n` is allocated and inserted inside a `BEGIN IMMEDIATE` SQLite transaction,
  so concurrent runs cannot reuse the same number.

### Notes

With `note`, the program creates:

```text
<data-directory>/<slug>-<N>-notes/README.md
```

`<slug>` is a portable ASCII slug derived from the title. Invalid characters are
replaced with `-`, trailing dashes are removed, Windows reserved names are
prefixed with `link-`, and the component is limited to 80 bytes. If two titles
produce the same slug, a stable numeric disambiguator is appended:

```text
collision-test-1-notes
collision-test-1-notes-2
```

The folder is created atomically. An existing folder or `README.md` is never
overwritten. If the link already has a `notes_path`, its files are left
untouched and the existing path is reported.

The initial `README.md` is written as UTF-8:

```markdown
# <page_name>

- **URL:** <link>
- **Created:** <local date and time>
- **Tags:**

## Notes


```

`notes_path` stores the absolute, resolved path. If folder creation fails, the
link row is kept without a notes path. If the path was created but the database
update fails, only the files created by that operation are removed.

## Data model

Schema version `1`, tracked with `PRAGMA user_version`:

```sql
CREATE TABLE IF NOT EXISTS links (
    id             INTEGER PRIMARY KEY,
    page_name      TEXT NOT NULL,
    page_name_key  TEXT NOT NULL,
    link           TEXT NOT NULL UNIQUE,
    tags           TEXT NOT NULL DEFAULT '',
    notes_path     TEXT,
    n              INTEGER NOT NULL CHECK (n >= 1),
    created_at     TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    UNIQUE (page_name_key, n)
);

CREATE INDEX IF NOT EXISTS idx_links_page_name
    ON links (page_name_key);
```

Initialisation is idempotent and does not destroy an existing database. A
database with a newer schema version is rejected rather than modified.

## Tests

```sh
make check
```

The suite uses a temporary `HOME`, a local fixture HTTP server and HTML
fixtures. It does not contact external services or write to the real user
profile. It covers:

- database and data-directory initialisation
- valid and invalid URL handling
- `<title>`, `og:title`, entity decoding and host fallback
- duplicate URLs and `n = 1`, `n = 2`
- notes creation, existing links without notes, and non-destructive repeats
- special characters and slug collisions
- filesystem failure without broken `notes_path` references
- the `lbr` alias
- concurrent inserts assigning unique `n` values

The permission-failure case is skipped when the suite runs as `root`, because
root bypasses Unix mode bits.

## Windows

Build with MSYS2/MinGW-w64 and the SQLite/libcurl packages, for example:

```sh
pacman -S mingw-w64-x86_64-gcc make \
          mingw-w64-x86_64-sqlite3 mingw-w64-x86_64-curl
make
```

The Makefile's `lbr` step uses `cp`, which is available in the MSYS2 shell. If
your environment does not provide it, copy `linkbrarian.exe` to `lbr.exe`
manually; the program does not depend on `argv[0]`. Paths use the native
separator and `%USERPROFILE%` is used for the home directory.

## Limits

This is the registration-and-notes MVP. It does not implement search, tag
editing, deletion, export, browser integration, cloud sync or automatic
transcription downloads.

Additional implementation notes:

- HTML is assumed to be UTF-8; no charset conversion is performed.
- `page_name_key` performs Unicode whitespace normalisation but only ASCII and
  Latin-1 case folding, so unusual scripts are compared case-sensitively.
- Slugs are ASCII, so titles written only in non-Latin scripts fall back to the
  name `link` (with collision disambiguators).
- URL comparison is conservative: only surrounding whitespace is trimmed.
