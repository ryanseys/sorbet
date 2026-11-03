#!/bin/bash

set -euo pipefail

# Sorbet must exit after this error, and must not hang.
if main/sorbet --silence-dev-message --lsp --dir test/cli/lsp-watchman-input-file test/cli/lsp-watchman-input-file/lsp-watchman-input-file.rb 2>&1; then
  echo "expected to fail, but it didn't!"
  exit 1
fi
