#!/usr/bin/env bash
set -euo pipefail

tmpdir="$(mktemp -d)"
cleanup() {
  rm -rf "$tmpdir"
}
trap cleanup exit

# This fake watchman reads one byte of each command, so that Sorbet's write cannot
# fail on a closed pipe, and then exits with no response.
fake_watchman="$tmpdir/watchman"
cat > "$fake_watchman" << 'EOF'
#!/bin/sh
head -c 1 > /dev/null
EOF
chmod +x "$fake_watchman"

# Keep stdin open, so that only the Watchman thread stops Sorbet.
in_pipe="$tmpdir/in_pipe"
mkfifo -m 600 "$in_pipe"

main/sorbet --silence-dev-message --lsp --watchman-path="$fake_watchman" --dir test/cli/lsp-watchman-exit-code < "$in_pipe" 2>&1 &
sorbet_pid=$!

# This should be
# exec {IN_FD}>"$in_pipe"
# but mac os has ancient version of bash.
exec 100>"$in_pipe"

set +e
wait "$sorbet_pid"
echo "exit code: $?"
