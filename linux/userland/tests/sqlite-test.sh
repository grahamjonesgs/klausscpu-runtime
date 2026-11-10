#!/bin/sh
# sqlite-test.sh [db]: sqlite-test.sql against a database file (default
# /data/sqlite-test.db: the persistent data image), then a second sqlite3
# process reopens it and reads it back.  The output must equal
# sqlite-test.expected; prints "sqlite-test: PASS" or the differences.
# Installed in the system image as /usr/share/klausscpu/tests/sqlite-test.sh.
D=$(dirname "$0")
db=${1:-/data/sqlite-test.db}
rm -f "$db" "$db-wal" "$db-shm"
{
	sqlite3 "$db" <"$D/sqlite-test.sql" 2>&1
	sqlite3 "$db" "SELECT 'reopen', count(*), sum(qty) FROM item; PRAGMA journal_mode;" 2>&1
} >/tmp/sqlite-test.out
if cmp -s /tmp/sqlite-test.out "$D/sqlite-test.expected"; then
	echo "sqlite-test: PASS ($(wc -l </tmp/sqlite-test.out) lines)"
else
	echo "sqlite-test: FAIL"
	diff "$D/sqlite-test.expected" /tmp/sqlite-test.out | head -40
fi
