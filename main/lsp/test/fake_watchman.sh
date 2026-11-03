#!/bin/bash
# Acts as the watchman CLI for watchman_process_test.cc. Like current Watchman releases, it refuses a subscription to a
# directory that it does not watch.
#
# $FAKE_WATCHMAN_STATE is a directory with these files:
#   watched         The fake adds each directory that it watches to this file, one on each line.
#   changes         Optional. The files that the first update after a subscription reports, one on each line.
#   refuse_watch    Optional. The watch command fails with the error in this file.
#   forget_watches  Optional. If present, the fake forgets each watch, as if `watchman watch-del` ran after it.
#   cancel          Optional. If present, the fake cancels the subscription after the first update.
set -euo pipefail

state="$FAKE_WATCHMAN_STATE"

# Prints $1 as a JSON string.
json_string() {
  local s="${1//\\/\\\\}"
  printf '"%s"' "${s//\"/\\\"}"
}

# Prints a response with the error $1.
error() {
  echo "{\"version\":\"fake\",\"error\":$(json_string "$1")}"
}

# Sorbet sends one command with no newline, and keeps the stdin of `watchman -p` open. The verb, the root and the
# subscription name come before the first `]`.
IFS= read -r -d ']' command || true

watch='^\["watch","([^"]*)"$'
subscribe='^\["subscribe","([^"]*)","([^"]*)",'

if [[ "$command" =~ $watch ]]; then
  root="${BASH_REMATCH[1]}"
  if [ "$*" != "-j --no-pretty" ]; then
    error "fake_watchman.sh: watch expects the arguments -j --no-pretty, not: $*"
    exit 1
  fi
  if [ -f "$state/refuse_watch" ]; then
    error "$(cat "$state/refuse_watch")"
    exit 1
  fi

  if [ ! -f "$state/forget_watches" ]; then
    echo "$root" >> "$state/watched"
  fi
  echo "{\"version\":\"fake\",\"watcher\":\"fake\",\"watch\":$(json_string "$root")}"
  exit 0
fi

if [[ "$command" =~ $subscribe ]]; then
  root="${BASH_REMATCH[1]}"
  name="$(json_string "${BASH_REMATCH[2]}")"
  if [ "$*" != "-j -p --no-pretty" ]; then
    error "fake_watchman.sh: subscribe expects the arguments -j -p --no-pretty, not: $*"
    exit 1
  fi

  if ! grep -qxF "$root" "$state/watched" 2> /dev/null; then
    error "watchman::RootResolveError: failed to resolve root: unable to resolve root $root: failed to resolve root: directory $root is not watched"
  else
    files=""
    if [ -f "$state/changes" ]; then
      while IFS= read -r file || [ -n "$file" ]; do
        files="$files${files:+,}$(json_string "$file")"
      done < "$state/changes"
    fi

    root="$(json_string "$root")"
    echo "{\"version\":\"fake\",\"subscribe\":$name,\"clock\":\"c:0:1\"}"
    echo "{\"version\":\"fake\",\"subscription\":$name,\"root\":$root,\"clock\":\"c:0:1\",\"files\":[],\"is_fresh_instance\":true,\"unilateral\":true}"
    echo "{\"version\":\"fake\",\"subscription\":$name,\"root\":$root,\"clock\":\"c:0:2\",\"files\":[$files],\"is_fresh_instance\":false,\"unilateral\":true}"
    if [ -f "$state/cancel" ]; then
      echo "{\"version\":\"fake\",\"unilateral\":true,\"canceled\":true,\"subscription\":$name,\"root\":$root}"
    fi
  fi

  # Keep stdout open until Sorbet closes stdin, so that Sorbet does not see the CLI exit.
  cat > /dev/null
  exit 0
fi

error "fake_watchman.sh: unknown command: $command"
exit 1
