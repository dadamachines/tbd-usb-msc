#!/usr/bin/env bash
# Reliable ESP-IDF build for this repo.
#   - Assumes direnv has activated the repo's pinned ESP-IDF (.envrc).
#   - Guard 1: regenerates the gitignored sdkconfig if it has drifted from the
#     tracked sdkconfig.defaults (ESP-IDF only regenerates when sdkconfig is
#     ABSENT — otherwise it silently keeps a stale one).
#   - Guard 2: fullcleans build/ if it was produced by a different IDF install.
# Usage: ./build.sh [extra idf.py build args]
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

if ! command -v idf.py >/dev/null 2>&1; then
  echo "❌ ESP-IDF not active. Run 'direnv allow' here, or source the pinned export.sh." >&2
  exit 1
fi

required_idf_path="${HOME}/esp/esp-idf-v6.0.1"
active_idf_version="$(idf.py --version 2>/dev/null)"
if [ "${IDF_PATH:-}" != "$required_idf_path" ] || [ "$active_idf_version" != "ESP-IDF v6.0.1" ]; then
  echo "❌ This repo requires ESP-IDF v6.0.1 at $required_idf_path." >&2
  echo "   Active: IDF_PATH=${IDF_PATH:-unset}, version=$active_idf_version" >&2
  exit 1
fi

# Guard 1 — stale gitignored sdkconfig vs tracked defaults
if [ -f sdkconfig ] && [ sdkconfig.defaults -nt sdkconfig ]; then
  echo "↻ sdkconfig is older than sdkconfig.defaults — regenerating from defaults"
  rm -f sdkconfig
fi

# Guard 2 — build/ was made with a different IDF install than the active one
if [ -f build/project_description.json ]; then
  built_idf=$(sed -n 's/.*"idf_path"[^"]*"\([^"]*\)".*/\1/p' build/project_description.json | head -1)
  if [ -n "${built_idf:-}" ] && [ -n "${IDF_PATH:-}" ] && [ "$built_idf" != "$IDF_PATH" ]; then
    echo "↻ build/ was made with $built_idf but active IDF is $IDF_PATH — fullclean"
    idf.py fullclean >/dev/null 2>&1 || rm -rf build
  fi
fi

echo "▶ $active_idf_version — building $(basename "$PWD")"
exec idf.py build "$@"
