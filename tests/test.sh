#!/bin/sh
BIN="${BIN:-build/mini_database}"
fails=0

check() {
    name=$1
    expected=$2
    actual=$("$BIN" 2>&1)
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
check "insert then select" \
"db > Executed.
db > (1, user1, person1@example.com)
Executed.
db > " <<'EOF'
insert 1 user1 person1@example.com
select
.exit
EOF

# ---- 2. table full ----
out=$( { seq 1 1401 | awk '{ print "insert " $1 " user" $1 " p" $1 "@x.com" }'
         echo .exit; } | "$BIN" 2>&1 )
last=$(printf '%s\n' "$out" | tail -n 2 | head -n 1)
if [ "$last" = "db > Error: Table full." ]; then
    echo "PASS  table full"
else
    echo "FAIL  table full"; printf 'got: %s\n' "$last"; fails=$((fails + 1))
fi

# ---- 3. max-length strings (finds the NUL bug) ----
U32=$(repeat_char a 32)
E255=$(repeat_char b 255)
check "max-length strings" \
"db > Executed.
db > (1, $U32, $E255)
Executed.
db > " <<EOF
insert 1 $U32 $E255
select
.exit
EOF

# ---- 4. strings too long ----
U33=$(repeat_char a 33)
E256=$(repeat_char b 256)
check "strings too long" \
"db > String is too long.
db > Executed.
db > " <<EOF
insert 1 $U33 $E256
select
.exit
EOF

# ---- 5. negative id ----
check "negative id" \
"db > ID must be positive.
db > Executed.
db > " <<'EOF'
insert -1 cstack foo@bar.com
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