#!/bin/sh
BIN="${BIN:-build/mini_database}"
fails=0

TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

compare() { # NAME EXPECTED ACTUAL
    if [ "$3" = "$2" ]; then
        echo "PASS  $1"
    else
        echo "FAIL  $1"
        printf -- '--- expected ---\n%s\n--- actual ---\n%s\n----------------\n' "$2" "$3"
        fails=$((fails + 1))
    fi
}

# check NAME EXPECTED DBFILE  < input
check() {
    compare "$1" "$2" "$("$BIN" "$3" 2>&1)"
}

repeat_char() {
    awk -v c="$1" -v n="$2" 'BEGIN { for (i = 0; i < n; i++) printf "%s", c }'
}

# ---- 1. insert then select ----
check "insert then select" \
"db > Executed.
db > (1, user1, person1@example.com)
Executed.
db > " "$TMPDIR/basic.db" <<'EOF'
insert 1 user1 person1@example.com
select
.exit
EOF

# ---- 2. max-length strings ----
U32=$(repeat_char a 32)
E255=$(repeat_char b 255)
check "max-length strings" \
"db > Executed.
db > (1, $U32, $E255)
Executed.
db > " "$TMPDIR/max_length.db" <<EOF
insert 1 $U32 $E255
select
.exit
EOF

# ---- 3. strings too long ----
U33=$(repeat_char a 33)
E256=$(repeat_char b 256)
check "strings too long" \
"db > String is too long.
db > Executed.
db > " "$TMPDIR/too_long.db" <<EOF
insert 1 $U33 $E256
select
.exit
EOF

# ---- 4. negative id ----
check "negative id" \
"db > ID must be positive.
db > Executed.
db > " "$TMPDIR/negative.db" <<'EOF'
insert -1 cstack foo@bar.com
select
.exit
EOF

# ---- 5. duplicate key ----
check "duplicate key" \
"db > Executed.
db > Error: Duplicate key.
db > (1, a, a@x.com)
Executed.
db > " "$TMPDIR/dup.db" <<'EOF'
insert 1 a a@x.com
insert 1 b b@x.com
select
.exit
EOF

# ---- 6. syntax error / unrecognized keyword ----
check "syntax and keyword errors" \
"db > Syntax error. Could not parse statement.
db > Unrecognized keyword at start of 'foo'.
db > " "$TMPDIR/errors.db" <<'EOF'
insert 1 onlyname
foo
.exit
EOF

# ---- 7. persistence: rows survive restart ----
DB="$TMPDIR/persist.db"
"$BIN" "$DB" >/dev/null 2>&1 <<'EOF'
insert 2 bob b@x.com
insert 1 alice a@x.com
.exit
EOF
check "persistence after restart" \
"db > (1, alice, a@x.com)
(2, bob, b@x.com)
Executed.
db > " "$DB" <<'EOF'
select
.exit
EOF

# ---- 8. first leaf split: .btree shape ----
IN="$TMPDIR/split.in"
{ seq 1 14 | awk '{ print "insert " $1 " user" $1 " p" $1 "@x.com" }'
  echo .btree; echo .exit; } > "$IN"
expected=$(awk 'BEGIN {
    for (i = 1; i <= 14; i++) print "db > Executed."
    print "db > Tree:"
    print "- internal (size 1)"
    print "  - leaf (size 7)"
    for (i = 1; i <= 7; i++) print "    - " i
    print "  - key 7"
    print "  - leaf (size 7)"
    for (i = 8; i <= 14; i++) print "    - " i
    printf "db > "
}')
check "leaf split btree shape" "$expected" "$TMPDIR/split.db" < "$IN"

# ---- 9. persistence of a split tree ----
expected=$(awk 'BEGIN {
    for (i = 1; i <= 14; i++)
        printf "%s(%d, user%d, p%d@x.com)\n", (i == 1 ? "db > " : ""), i, i, i
    print "Executed."
    printf "db > "
}')
check "split tree survives restart" "$expected" "$TMPDIR/split.db" <<'EOF'
select
.exit
EOF

# ---- 10. many out-of-order inserts come back sorted (internal splits) ----
# i*37 mod 211 is a permutation of 1..210 because 211 is prime
IN="$TMPDIR/many.in"
{ seq 1 210 | awk '{ id = ($1 * 37) % 211; print "insert " id " user" id " p" id "@x.com" }'
  echo select; echo .exit; } > "$IN"
expected=$(awk 'BEGIN {
    for (i = 1; i <= 210; i++) print "db > Executed."
    for (i = 1; i <= 210; i++)
        printf "%s(%d, user%d, p%d@x.com)\n", (i == 1 ? "db > " : ""), i, i, i
    print "Executed."
    printf "db > "
}')
check "210 shuffled inserts select sorted" "$expected" "$TMPDIR/many.db" < "$IN"

# ---- summary ----
echo
if [ "$fails" -eq 0 ]; then
    echo "All tests passed."
else
    echo "$fails test(s) failed."
    exit 1
fi
