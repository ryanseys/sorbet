#!/bin/bash
# Stands in for the `watchman` CLI in watchman_process_test.cc. Like Watchman, it only accepts a
# `subscribe` for a root that `watch-project` has already watched.
#
# $FAKE_WATCHMAN_STATE is a directory that holds the state of the fake:
#   watched       the roots that `watch-project` watched, one per line (written by this script)
#   project_root  if present, `watch-project` reports this directory as the root it watches
#   refuse        if present, `watch-project` fails with this error
#   changes       if present, the files to report as changed after a `subscribe`, one per line,
#                 relative to the watched root
set -euo pipefail

state="$FAKE_WATCHMAN_STATE"

# The CLI reads a single command from stdin.
IFS= read -r command || true

watch_project='^\["watch-project","([^"]*)"\]$'
subscribe='^\["subscribe","([^"]*)","([^"]*)",'
relative_root='"relative_root":"([^"]*)"'

if [[ "$command" =~ $watch_project ]]; then
  workspace="$(cd "${BASH_REMATCH[1]}" && pwd -P)"
  if [ -f "$state/refuse" ]; then
    echo "{\"version\":\"fake\",\"error\":\"$(cat "$state/refuse")\"}"
    exit 1
  fi

  root="$workspace"
  if [ -f "$state/project_root" ]; then
    root="$(cat "$state/project_root")"
  fi
  echo "$root" >> "$state/watched"

  if [ "$root" = "$workspace" ]; then
    echo "{\"version\":\"fake\",\"watcher\":\"fake\",\"watch\":\"$root\"}"
  else
    echo "{\"version\":\"fake\",\"watcher\":\"fake\",\"watch\":\"$root\",\"relative_path\":\"${workspace#"$root"/}\"}"
  fi
  exit 0
fi

if [[ "$command" =~ $subscribe ]]; then
  root="${BASH_REMATCH[1]}"
  name="${BASH_REMATCH[2]}"
  if ! grep -qxF "$root" "$state/watched" 2> /dev/null; then
    echo "{\"version\":\"fake\",\"error\":\"watchman::RootResolveError: failed to resolve root: unable to resolve root $root: failed to resolve root: directory $root is not watched\"}"
  else
    prefix=""
    if [[ "$command" =~ $relative_root ]]; then
      prefix="${BASH_REMATCH[1]}/"
    fi

    # Like Watchman, report names relative to `relative_root`, and only the files inside it.
    files=""
    if [ -f "$state/changes" ]; then
      while IFS= read -r file; do
        case "$file" in
          "$prefix"*) files="$files${files:+,}\"${file#"$prefix"}\"" ;;
        esac
      done < "$state/changes"
    fi

    echo "{\"version\":\"fake\",\"subscribe\":\"$name\",\"clock\":\"c:0:1\"}"
    echo "{\"version\":\"fake\",\"subscription\":\"$name\",\"root\":\"$root\",\"clock\":\"c:0:1\",\"files\":[],\"is_fresh_instance\":true,\"unilateral\":true}"
    echo "{\"version\":\"fake\",\"subscription\":\"$name\",\"root\":\"$root\",\"clock\":\"c:0:2\",\"files\":[$files],\"is_fresh_instance\":false,\"unilateral\":true}"
  fi

  # Like `watchman -p`, keep running until the client closes stdin.
  cat > /dev/null
  exit 0
fi

echo "{\"version\":\"fake\",\"error\":\"unknown command: $command\"}"
exit 1
