#!/bin/sh
# linkbrarian test suite. Uses a temporary HOME and a local fixture server.
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd) || exit 1
BIN="$ROOT/linkbrarian"
LBR="$ROOT/lbr"
HTTPD="$ROOT/tests/httpd"
FIXTURES="$ROOT/tests/fixtures"

TMP=$(mktemp -d "${TMPDIR:-/tmp}/linkbrarian-test.XXXXXX") || exit 1
SRV=
OUT="$TMP/stdout"
ERR="$TMP/stderr"
ST=0
PASS=0
FAILED=0

cleanup()
{
	if [ -n "$SRV" ]; then
		kill "$SRV" 2>/dev/null || true
	fi
	rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

fail()
{
	printf 'FAIL: %s\n' "$*" >&2
	FAILED=$((FAILED + 1))
	exit 1
}

ok()
{
	printf 'ok - %s\n' "$*"
	PASS=$((PASS + 1))
}

run()
{
	"$@" >"$OUT" 2>"$ERR"
	ST=$?
	return 0
}

expect_ok()
{
	if [ "$ST" -ne 0 ]; then
		fail "$1: expected success, got $ST; stderr: $(cat "$ERR")"
	fi
}

expect_fail()
{
	if [ "$ST" -eq 0 ]; then
		fail "$1: expected failure, got success"
	fi
}

out_has()
{
	grep -qF -- "$1" "$OUT" || fail "$2: stdout does not contain '$1'"
}

err_has()
{
	grep -qF -- "$1" "$ERR" || fail "$2: stderr does not contain '$1'"
}

new_home()
{
	HOME="$TMP/home-$1"
	export HOME
	rm -rf "$HOME"
	mkdir -p "$HOME"
}

dbq()
{
	sqlite3 "$HOME/.linkbrarian/linkography.db" "$1"
}

count_links()
{
	dbq "SELECT count(*) FROM links;"
}

name_for()
{
	dbq "SELECT page_name FROM links WHERE link = '$1';"
}

make -C "$ROOT" >/dev/null 2>&1 || {
	printf 'FAIL: build failed\n' >&2
	exit 1
}

"$HTTPD" 0 "$FIXTURES" >"$TMP/port" 2>"$TMP/httpd.log" &
SRV=$!
tries=0
while [ ! -s "$TMP/port" ]; do
	tries=$((tries + 1))
	if [ "$tries" -gt 100 ]; then
		cat "$TMP/httpd.log" >&2
		fail "fixture server did not start"
	fi
	sleep 0.05
done
PORT=$(cat "$TMP/port")
BASE="http://127.0.0.1:$PORT"

# ---------------------------------------------------------------- 1: init
new_home init
run "$BIN" "$BASE/title.html"
expect_ok "register a valid URL"
[ -d "$HOME/.linkbrarian" ] || fail "data directory was not created"
[ -f "$HOME/.linkbrarian/linkography.db" ] || fail "database was not created"
[ "$(dbq 'PRAGMA user_version;')" = "1" ] || fail "schema version is not 1"
[ "$(count_links)" = "1" ] || fail "expected one link"
row=$(dbq "SELECT page_name || '|' || link || '|' || tags || '|' || COALESCE(notes_path, 'NULL') || '|' || n FROM links;")
[ "$row" = "Example & Article|$BASE/title.html||NULL|1" ] ||
	fail "unexpected row: $row"
out_has "[+] Link saved: Example & Article" "save output"
out_has "URL: $BASE/title.html" "save output"
ok "database initialization and valid URL registration"

# ------------------------------------------------------------ 2: CLI help
run "$BIN" -h
expect_ok "help exits successfully"
out_has "usage: linkbrarian" "help output"
run "$BIN" -v
expect_ok "version exits successfully"
[ "$(cat "$OUT")" = "0.1.0" ] || fail "unexpected version output"
run "$BIN" -z
expect_fail "unknown option is rejected"
run "$BIN"
expect_fail "missing URL is rejected"
ok "help, version and usage errors"

# ------------------------------------------- 3: invalid URLs and arguments
new_home invalid
for u in "example.com" "ftp://example.com/a" "http://" "https://exa mple.com" \
	 "javascript:alert(1)" "://example.com"; do
	run "$BIN" "$u"
	expect_fail "invalid URL rejected: $u"
done
run "$BIN" "$BASE/title.html" bad
expect_fail "unknown second argument is rejected"
run "$BIN" "$BASE/title.html" note extra
expect_fail "third positional argument is rejected"
if [ -f "$HOME/.linkbrarian/linkography.db" ]; then
	[ "$(count_links)" = "0" ] || fail "invalid input created rows"
fi
ok "invalid URLs and extra arguments are rejected"

# --------------------------------------------------------- 4: host fallback
new_home fallback
run "$BIN" "$BASE/notitle.html"
expect_ok "missing title still saves the link"
[ "$(name_for "$BASE/notitle.html")" = "127.0.0.1" ] ||
	fail "expected host fallback for missing title"
run "$BIN" "$BASE/empty-title.html"
expect_ok "empty title still saves the link"
[ "$(name_for "$BASE/empty-title.html")" = "127.0.0.1" ] ||
	fail "expected host fallback for empty title"
run "$BIN" "$BASE/plain.txt"
expect_ok "non-HTML response still saves the link"
[ "$(name_for "$BASE/plain.txt")" = "127.0.0.1" ] ||
	fail "expected host fallback for non-HTML response"
run "$BIN" "http://127.0.0.1:$((PORT + 1))/no-server"
expect_ok "unreachable host still saves the link"
[ "$(name_for "http://127.0.0.1:$((PORT + 1))/no-server")" = "127.0.0.1" ] ||
	fail "expected host fallback for unreachable server"
ok "host fallback when HTTP or title extraction fails"

# ---------------------------------------------------- 5: title extraction
new_home titles
run "$BIN" "$BASE/title.html"
expect_ok "title page saved"
[ "$(name_for "$BASE/title.html")" = "Example & Article" ] ||
	fail "HTML title was not trimmed or decoded"
run "$BIN" "$BASE/og.html"
expect_ok "og:title page saved"
[ "$(name_for "$BASE/og.html")" = 'OG Fallback "Title"' ] ||
	fail "og:title was not used correctly"
run "$BIN" "$BASE/og-reordered.html"
expect_ok "og:title with reordered attributes saved"
[ "$(name_for "$BASE/og-reordered.html")" = "Reordered OG" ] ||
	fail "og:title attribute order handling failed"
run "$BIN" "$BASE/og-name.html"
expect_ok "og:title via name attribute saved"
[ "$(name_for "$BASE/og-name.html")" = "Named OG" ] ||
	fail "name=og:title handling failed"
run "$BIN" "$BASE/case.html"
expect_ok "uppercase tags saved"
[ "$(name_for "$BASE/case.html")" = "Case & Tags" ] ||
	fail "case-insensitive tag handling failed"
run "$BIN" "$BASE/entities.html"
expect_ok "entity title saved"
[ "$(name_for "$BASE/entities.html")" = "Café — Guide" ] ||
	fail "numeric HTML entities were not decoded"
run "$BIN" "$BASE/inline.html"
expect_ok "title with inline markup saved"
[ "$(name_for "$BASE/inline.html")" = "Inline Bold Title" ] ||
	fail "inline tags inside title were not stripped"
ok "title and og:title extraction"

# ------------------------------------------------------- 6: duplicate URL
new_home duplicate
run "$BIN" "$BASE/title.html"
expect_ok "first save"
run "$BIN" "$BASE/title.html"
expect_ok "second save is idempotent"
[ "$(count_links)" = "1" ] || fail "duplicate URL created another row"
out_has "[=] Link already registered: Example & Article" "duplicate output"
ok "duplicate URLs do not create rows"

# ------------------------------------------------------------ 7: numbering
new_home numbering
run "$BIN" "$BASE/n-a.html"
expect_ok "first shared name saved"
run "$BIN" "$BASE/n-b.html"
expect_ok "second shared name saved"
[ "$(dbq "SELECT n FROM links WHERE link = '$BASE/n-a.html';")" = "1" ] ||
	fail "first shared name did not get n=1"
[ "$(dbq "SELECT n FROM links WHERE link = '$BASE/n-b.html';")" = "2" ] ||
	fail "second shared name did not get n=2"
[ "$(dbq 'SELECT count(DISTINCT page_name_key) FROM links;')" = "1" ] ||
	fail "shared names were not grouped by page_name_key"
ok "same title gets n=1, n=2 for different URLs"

# --------------------------------------------------------- 8: create notes
new_home note
run "$BIN" "$BASE/title.html" note
expect_ok "save with notes"
notes=$(dbq "SELECT notes_path FROM links WHERE link = '$BASE/title.html';")
[ -n "$notes" ] || fail "notes_path was not stored"
case "$notes" in
/*) ;;
*) fail "notes_path is not absolute: $notes" ;;
esac
[ -d "$notes" ] || fail "notes directory does not exist: $notes"
[ -f "$notes/README.md" ] || fail "README.md was not created: $notes"
[ "$(basename "$notes")" = "example-article-1-notes" ] ||
	fail "unexpected notes directory name: $(basename "$notes")"
grep -qF "# Example & Article" "$notes/README.md" || fail "README title missing"
grep -qF -- "- **URL:** $BASE/title.html" "$notes/README.md" || fail "README URL missing"
grep -qF -- "- **Created:**" "$notes/README.md" || fail "README timestamp missing"
grep -qF -- "- **Tags:**" "$notes/README.md" || fail "README tags missing"
grep -qF "## Notes" "$notes/README.md" || fail "README notes section missing"
out_has "[+] Notes created: $notes" "notes output"
ok "note creates the folder and README.md"

# --------------------------------- 9: note for an already registered link
new_home existing-note
run "$BIN" "$BASE/og.html"
expect_ok "register without notes"
[ "$(dbq "SELECT notes_path IS NULL FROM links WHERE link = '$BASE/og.html';")" = "1" ] ||
	fail "notes_path should be NULL before note"
run "$BIN" "$BASE/og.html" note
expect_ok "add notes to existing link"
notes=$(dbq "SELECT notes_path FROM links WHERE link = '$BASE/og.html';")
[ -d "$notes" ] || fail "existing link notes directory missing"
[ -f "$notes/README.md" ] || fail "existing link README missing"
ok "note works for a previously registered link"

# ------------------------------------------ 10: repeated note is non-destructive
printf '\nUSER CONTENT\n' >>"$notes/README.md"
run "$BIN" "$BASE/og.html" note
expect_ok "repeat note"
[ "$(dbq "SELECT notes_path FROM links WHERE link = '$BASE/og.html';")" = "$notes" ] ||
	fail "repeat note changed notes_path"
grep -qF "USER CONTENT" "$notes/README.md" || fail "repeat note overwrote README.md"
out_has "[=] Notes already exist: $notes" "repeat note output"
ok "repeated note keeps existing files"

# ----------------------------------------- 11: special names and slug collisions
new_home special
run "$BIN" "$BASE/special.html" note
expect_ok "special title note"
notes=$(dbq "SELECT notes_path FROM links WHERE link = '$BASE/special.html';")
base=$(basename "$notes")
case "$base" in
.*) fail "slug is hidden: $base" ;;
*..*) fail "slug contains traversal: $base" ;;
esac
[ "$base" = "con-weird-name-1-notes" ] || fail "unexpected special slug: $base"
[ -f "$notes/README.md" ] || fail "special README missing"

new_home collision
run "$BIN" "$BASE/collide-a.html" note
expect_ok "first colliding slug note"
run "$BIN" "$BASE/collide-b.html" note
expect_ok "second colliding slug note"
pa=$(dbq "SELECT notes_path FROM links WHERE link = '$BASE/collide-a.html';")
pb=$(dbq "SELECT notes_path FROM links WHERE link = '$BASE/collide-b.html';")
[ "$pa" != "$pb" ] || fail "colliding slugs shared a directory"
[ -d "$pa" ] && [ -d "$pb" ] || fail "colliding slug directories missing"
[ "$(basename "$pa")" = "collision-test-1-notes" ] ||
	fail "unexpected first collision name: $(basename "$pa")"
[ "$(basename "$pb")" = "collision-test-1-notes-2" ] ||
	fail "unexpected desambiguated collision name: $(basename "$pb")"
ok "special characters and slug collisions"

# -------------------------------------------------- 12: write failure safety
new_home failures
HOME=/proc/self
export HOME
run "$BIN" "$BASE/title.html"
expect_fail "unwritable home is reported"
err_has "data directory" "unwritable home error"
HOME="$TMP/home-failures"
export HOME

# Force notes creation to fail after the link row was inserted by exhausting
# every desambiguated candidate name. The row must keep notes_path NULL.
run "$BIN" "$BASE/title.html"
expect_ok "register link before forcing note failure"
i=0
while [ "$i" -lt 100 ]; do
	if [ "$i" -eq 0 ]; then
		candidate="example-article-1-notes"
	else
		candidate="example-article-1-notes-$((i + 1))"
	fi
	mkdir "$HOME/.linkbrarian/$candidate" || fail "cannot prepare notes collision"
	i=$((i + 1))
done
run "$BIN" "$BASE/title.html" note
expect_fail "notes creation failure is reported"
err_has "cannot create notes" "notes failure error"
[ "$(count_links)" = "1" ] || fail "link row disappeared after note failure"
[ "$(dbq 'SELECT notes_path IS NULL FROM links;')" = "1" ] ||
	fail "notes_path points at a directory that was not created"
ok "notes filesystem failure leaves no broken database reference"

# Permission failures only bite non-root users; root bypasses mode bits.
new_home permissions
run "$BIN" "$BASE/collide-a.html"
expect_ok "register link before permission test"
if [ "$(id -u)" -ne 0 ]; then
	chmod 0555 "$HOME/.linkbrarian"
	run "$BIN" "$BASE/collide-a.html" note
	chmod 0755 "$HOME/.linkbrarian"
	expect_fail "read-only data directory is reported"
	[ "$(dbq "SELECT notes_path IS NULL FROM links WHERE link = '$BASE/collide-a.html';")" = "1" ] ||
		fail "read-only notes failure stored a path"
	ok "permission failure leaves no broken database reference"
else
	ok "skipped permission failure test (running as root)"
fi

# ---------------------------------------------------- 13: lbr alias parity
new_home alias
run "$LBR" "$BASE/title.html"
expect_ok "lbr alias runs"
[ "$(count_links)" = "1" ] || fail "lbr alias did not use the same database"
out_has "[+] Link saved: Example & Article" "alias output"
ok "lbr alias shares behaviour and storage"

# --------------------------------------- 14: concurrent numbering is safe
new_home concurrent
pids=
i=1
while [ "$i" -le 6 ]; do
	"$BIN" "$BASE/title.html?i=$i" >"$TMP/c$i.out" 2>"$TMP/c$i.err" &
	pids="$pids $!"
	i=$((i + 1))
done
for pid in $pids; do
	if ! wait "$pid"; then
		cat "$TMP"/c*.err >&2
		fail "concurrent client $pid failed"
	fi
done
[ "$(count_links)" = "6" ] || fail "concurrent inserts lost rows"
[ "$(dbq 'SELECT count(DISTINCT n) FROM links;')" = "6" ] ||
	fail "concurrent inserts reused an n value"
[ "$(dbq "SELECT min(n) || ',' || max(n) FROM links;")" = "1,6" ] ||
	fail "concurrent numbering is not 1..6"
ok "concurrent inserts assign unique n values"

printf '\n%d passed, %d failed\n' "$PASS" "$FAILED"
[ "$FAILED" -eq 0 ] || exit 1
