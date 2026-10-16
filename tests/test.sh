#!/bin/sh
BIN="${BIN:-build/mini_database}"

echo "=== insert then select ==="
"$BIN" <<'EOF'
insert 1 alice alice@example.com
select
.exit
EOF

echo
echo "=== syntax error (missing email) ==="
"$BIN" <<'EOF'
insert 1 alice
select
.exit
EOF

echo
echo "=== unrecognized command ==="
"$BIN" <<'EOF'
frobnicate
.exit
EOF