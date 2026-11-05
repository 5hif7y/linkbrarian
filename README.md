# linkbrarian

**linkbrarian** is a small local-first command-line tool for saving links and
keeping a Markdown notes folder next to each one. It stores its catalogue in
SQLite under the user's home directory, independent of the current working
directory.

The implementation is deliberately compact: one C99 translation unit, a small
`config.h`, SQLite for storage and libcurl for fetching page metadata. There is
no daemon, no browser, no JavaScript execution and no network sync.

```sh
linkbrarian "https://example.com/article"
lbr         "https://example.com/article"

linkbrarian "https://example.com/article" note
lbr         "https://example.com/article" note
```

`linkbrarian` and `lbr` are equivalent commands; both are built from the same
source and use the same database.

## Requirements

- CMake 3.16 or newer
- A C compiler:
  - GCC or Clang for Unix-like systems
  - MSVC 2019 16.7+ on Windows (C11 mode is selected for MSVC)
- SQLite 3 development files
- libcurl development files
- utf8proc development files (Unicode normalization and case folding)
- `sqlite3` command-line tool, only for the test suite

System package examples:

```sh
# Alpine
apk add build-base cmake sqlite sqlite-dev curl-dev utf8proc-dev

# Debian / Ubuntu
apt install build-essential cmake libsqlite3-dev libcurl4-openssl-dev \
            libutf8proc-dev sqlite3

# macOS (Homebrew)
brew install cmake sqlite curl utf8proc
```

On Windows, install the dependencies with vcpkg:

```powershell
vcpkg install sqlite3:x64-windows curl:x64-windows utf8proc:x64-windows
```

## Build

Unix-like systems:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

To build with Clang instead of the default compiler:

```sh
cmake -S . -B build -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Windows with MSVC and vcpkg:

```powershell
cmake -S . -B build -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
cmake --build build --config Release
```

The build produces:

| Platform | Main executable | Alias |
|---|---|---|
| Unix-like | `build/linkbrarian` | `build/lbr` |
| Windows | `build\Release\linkbrarian.exe` | `build\Release\lbr.exe` |

The optional test fixture server is built only on Unix-like systems. CMake also
works with the Ninja generator, for example
`cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`.

## Install

Unix-like systems:

```sh
cmake --install build --prefix "$HOME/.local"
```

Windows:

```powershell
cmake --install build --config Release --prefix C:\Tools\linkbrarian
```

Both `linkbrarian` and `lbr` are installed. With CMake 3.21 or newer, the vcpkg
runtime DLLs are copied next to the executables automatically. Add the chosen
`bin` directory to `PATH` if necessary.

## Usage

```text
linkbrarian [-hv] URL [note]
lbr         [-hv] URL [note]
```

- `URL` must be an absolute `http://` or `https://` URL. Surrounding
  whitespace is ignored; query strings and fragments are preserved.
- `note` is the only optional positional argument. It creates a notes folder
  and an initial `README.md`.
- Extra positional arguments are rejected.

Options:

```text
-h, --help     show help
-v, --version  show version
```

Normal output goes to `stdout`; errors go to `stderr`.

### Save a link

```sh
linkbrarian "https://odin-lang.org/docs/overview/"
```

Example output:

```text
[+] Link saved: Overview | Odin Programming Language
    URL: https://odin-lang.org/docs/overview/
```

At this point the link is in the database, but no notes folder has been
created.

### Add notes to the same link

```sh
linkbrarian "https://odin-lang.org/docs/overview/" note
```

Example output:

```text
[=] Link already registered: Overview | Odin Programming Language
    URL: https://odin-lang.org/docs/overview/
[+] Notes created: /home/you/.linkbrarian/overview-odin-programming-language-1-notes
```

The notes operation is safe to repeat. Existing files are never overwritten:

```text
[=] Notes already exist: /home/you/.linkbrarian/overview-odin-programming-language-1-notes
```

### Register and document the example links

These popular documentation links are useful for trying the complete workflow.
The first command registers a link without notes; the others register and create
their notes folders in one step:

```sh
linkbrarian "https://odin-lang.org/docs/overview/"

lbr "https://en.cppreference.com/c/language" note
lbr "https://docs.python.org/3/extending/index.html" note
lbr "https://www.lua.org/manual/5.5/" note
lbr "https://docs.kicad.org/10.0/en/getting_started_in_kicad/getting_started_in_kicad.html" note
lbr "https://wiki.freecad.org/Getting_started" note
```

Titles are fetched when a URL is first registered. If a site is temporarily
unreachable or does not provide a usable HTML title, the host is stored instead
(for example `www.lua.org`); the URL is still saved and notes still work.

To assign the notes folder to the first link afterwards:

```sh
lbr "https://odin-lang.org/docs/overview/" note
```

### Inspect the catalogue

Unix-like systems:

```sh
sqlite3 "$HOME/.linkbrarian/linkography.db" \
  "SELECT id, n, page_name, notes_path FROM links ORDER BY id;"
```

PowerShell:

```powershell
sqlite3 "$env:USERPROFILE\.linkbrarian\linkography.db" `
  "SELECT id, n, page_name, notes_path FROM links ORDER BY id;"
```

### Edit notes

The generated note is an ordinary Markdown file. Open it with any editor:

```sh
$EDITOR "$HOME/.linkbrarian/overview-odin-programming-language-1-notes/README.md"
```

Running `note` again will not replace your text. The database only stores the
folder path; images, transcripts and other files can be added to that folder
manually.

### Exit codes

| Code | Meaning |
|---:|---|
| `0` | link saved, or the link was already registered |
| `1` | invalid command-line arguments |
| `2` | invalid URL |
| `3` | storage or database error |
| `4` | notes folder could not be created |

## Storage

Data lives in the user's home directory:

| System | Data directory | Database |
|---|---|---|
| Windows | `%USERPROFILE%\.linkbrarian\` | `%USERPROFILE%\.linkbrarian\linkography.db` |
| Unix-like | `$HOME/.linkbrarian/` | `$HOME/.linkbrarian/linkography.db` |

`.linkbrarian` is created automatically with mode `0700` on Unix-like systems.
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
- Grouping uses `page_name_key`: the title is normalised with Unicode
  NFKC case folding, Unicode whitespace is collapsed and surrounding whitespace
  is removed. The visible `page_name` keeps its original casing, compatibility
  forms and interior spacing.
- `n` is allocated and inserted inside a `BEGIN IMMEDIATE` SQLite transaction,
  so concurrent runs cannot reuse the same number.

### Notes

With `note`, the program creates:

```text
<data-directory>/<slug>-<N>-notes/README.md
```

`<slug>` is a portable, lower-case Unicode slug derived from the title
(NFKC case folding). Letters, numbers and combining marks are kept; separators,
punctuation, symbols and control characters become `-`. Trailing dashes are
removed, Windows reserved names are prefixed with `link-`, and the component is
limited to 80 UTF-8 bytes. If two titles produce the same slug, a stable numeric
disambiguator is appended:

```text
collision-test-1-notes
collision-test-1-notes-2
москва-1-notes
東京-1-notes
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

On Windows, filesystem operations convert UTF-8 paths to UTF-16 and use the
wide-character APIs, so Unicode slugs work independently of the active ANSI
code page.

## Data model

Schema version `2`, tracked with `PRAGMA user_version`:

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

Databases created with schema version `1` are migrated in place on first open:

- `page_name_key` is recomputed for every row with NFKC case folding.
- `n` is renumbered sequentially per new key, preserving row ids and
  `created_at`.
- `notes_path` is preserved exactly; existing note folders are not renamed.

## Tests

Unix-like systems:

```sh
ctest --test-dir build --output-on-failure
```

The suite uses a temporary `HOME`, a local fixture HTTP server and HTML
fixtures. It does not contact external services or write to the real user
profile. It covers:

- database and data-directory initialisation
- valid and invalid URL handling
- `<title>`, `og:title`, entity decoding and host fallback
- duplicate URLs and `n = 1`, `n = 2`
- NFKC case folding for Unicode-equivalent titles
- Unicode notes slugs (Cyrillic, CJK, emoji)
- migration of schema version `1` databases to version `2`
- notes creation, existing links without notes, and non-destructive repeats
- special characters and slug collisions
- filesystem failure without broken `notes_path` references
- the `lbr` alias
- concurrent inserts assigning unique `n` values

The test harness is POSIX shell and is enabled only when CMake runs on a
Unix-like system. It also needs the `sqlite3` command-line tool. The
permission-failure case is skipped when the suite runs as `root`, because root
bypasses Unix mode bits.

## Windows

1. Install CMake, Visual Studio with the C workload, and vcpkg.
2. Install `sqlite3:x64-windows` and `curl:x64-windows` with vcpkg.
3. Configure with the vcpkg toolchain file, build with `--config Release`, and
   optionally install as shown above.

The Windows build uses the native path separator and `%USERPROFILE%` for the
home directory. Filesystem operations use UTF-16 wide-character APIs, so
Unicode note folder names do not depend on the active ANSI code page.
`linkbrarian.exe` and `lbr.exe` are separate executables; the program does not
depend on `argv[0]`. The automated fixture tests are not built on Windows
because the harness is POSIX shell.

## Limits

This is the registration-and-notes MVP. It does not implement search, tag
editing, deletion, export, browser integration, cloud sync or automatic
transcription downloads.

Additional implementation notes:

- HTML is assumed to be UTF-8; no charset conversion is performed.
- URL comparison is conservative by design: only surrounding whitespace is
  trimmed. Query strings, fragments and trailing slashes are preserved, so
  trivially different spellings may be stored as separate links.
- Notes are ordinary files owned by the user; the database stores only their
  folder path.
