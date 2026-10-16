#!/bin/sh
BIN="${BIN:-build/mini_database}"
fails=0

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

# check NAME EXPECTED DBFILE  < input
# Runs the db with DBFILE as argv[1] and stdin as input; compares output to EXPECTED.
check() {
    name=$1
    expected=$2
    dbfile=$3
    actual=$("$BIN" "$dbfile" 2>&1)
    if [ "$actual" = "$expected" ]; then
        echo "PASS  $name"
    else
        echo "FAIL  $name"
        printf -- '--- expected ---\n%s\n--- actual ---\n%s\n----------------\n' \
            "$expected" "$actual"
        fails=$((fails + 1))
    fi
}

repeat_char() {
    awk -v c="$1" -v n="$2" 'BEGIN { for (i = 0; i < n; i++) printf "%s", c }'
}

# ---- 1. insert then select ----
DB="$TMPDIR/insert_select.db"
check "insert then select" \
"db > Executed.
db > (1, user1, person1@example.com)
Executed.
db > " "$DB" <<'EOF'
insert 1 user1 person1@example.com
select
.exit
EOF

# ---- 2. table full ----
DB="$TMPDIR/table_full.db"
out=$( { seq 1 1401 | awk '{ print "insert " $1 " user" $1 " p" $1 "@x.com" }'
         echo .exit; } | "$BIN" "$DB" 2>&1 )
last=$(printf '%s\n' "$out" | tail -n 2 | head -n 1)
if [ "$last" = "db > Error: Table full." ]; then
    echo "PASS  table full"
else
    echo "FAIL  table full"; printf 'got: %s\n' "$last"; fails=$((fails + 1))
fi

# ---- 3. max-length strings ----
U32=$(repeat_char a 32)
E255=$(repeat_char b 255)
DB="$TMPDIR/max_length.db"
check "max-length strings" \
"db > Executed.
db > (1, $U32, $E255)
Executed.
db > " "$DB" <<EOF
insert 1 $U32 $E255
select
.exit
EOF

# ---- 4. strings too long ----
U33=$(repeat_char a 33)
E256=$(repeat_char b 256)
DB="$TMPDIR/too_long.db"
check "strings too long" \
"db > String is too long.
db > Executed.
db > " "$DB" <<EOF
insert 1 $U33 $E256
select
.exit
EOF

# ---- 5. negative id ----
DB="$TMPDIR/negative_id.db"
check "negative id" \
"db > ID must be positive.
db > Executed.
db > " "$DB" <<'EOF'
insert -1 cstack foo@bar.com
select
.exit
EOF

# ---- 6. persistence: insert, exit, reopen, select ----
DB="$TMPDIR/persistence.db"
rm -f "$DB"

check "persistence: insert" \
"db > Executed.
db > " "$DB" <<'EOF'
insert 1 user1 person1@example.com
.exit
EOF

check "persistence: select after restart" \
"db > (1, user1, person1@example.com)
Executed.
db > " "$DB" <<'EOF'
select
.exit
EOF

# ---- 7. persistence: multiple rows survive restart ----
DB="$TMPDIR/persistence_multi.db"
rm -f "$DB"

check "persistence: insert two rows" \
"db > Executed.
db > Executed.
db > " "$DB" <<'EOF'
insert 1 alice a@x.com
insert 2 bob b@x.com
.exit
EOF

check "persistence: both rows after restart" \
"db > (1, alice, a@x.com)
(2, bob, b@x.com)
Executed.
db > " "$DB" <<'EOF'
select
.exit
EOF

# ---- summary ----
echo
if [ "$fails" -eq 0 ]; then
    echo "All tests passed."
else
    echo "$fails test(s) failed."
    exit 1
fi