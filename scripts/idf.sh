#!/usr/bin/env bash
# scripts/idf.sh — wrapper that sources ESP-IDF's export.sh and forwards
# all remaining arguments to idf.py.
#
# Why this exists
# ---------------
# Running idf.py requires sourcing ESP-IDF's export.sh first so that
# IDF_PATH, the Python virtualenv PATH, and the cross-toolchain PATH
# entries are in place. The canonical invocation is:
#
#     . ~/esp/esp-idf-v5.5/export.sh && idf.py build
#
# The leading `. ` (POSIX `source` shorthand) is an unbounded command
# prefix from a sandbox-whitelist standpoint — permitting it in an
# agent's allow-list effectively permits running any script. This
# wrapper lets the sandbox see a concrete, namespaced invocation like
# `./scripts/idf.sh build` which can be whitelisted tightly (for
# example as `./scripts/idf.sh *`) without opening a hole.
#
# Usage
# -----
#     ./scripts/idf.sh build
#     ./scripts/idf.sh set-target esp32c5
#     ./scripts/idf.sh fullclean
#     ./scripts/idf.sh -p /dev/cu.usbmodem* flash monitor
#     ./scripts/idf.sh -p /dev/cu.usbserial-* flash monitor
#
# By default the wrapper sources ~/esp/esp-idf-v5.5/export.sh. If you
# have ESP-IDF installed elsewhere (for example the v5.3 install still
# sitting at ~/esp/esp-idf/), override with IDF_EXPORT:
#
#     IDF_EXPORT=~/esp/esp-idf/export.sh ./scripts/idf.sh build

set -euo pipefail

IDF_EXPORT="${IDF_EXPORT:-$HOME/esp/esp-idf-v5.5/export.sh}"

if [[ ! -f "$IDF_EXPORT" ]]; then
    echo "error: ESP-IDF export.sh not found at $IDF_EXPORT" >&2
    echo "       set IDF_EXPORT=/path/to/export.sh to override" >&2
    exit 1
fi

# shellcheck disable=SC1090  # sourcing a variable-expanded path is intentional
. "$IDF_EXPORT" >/dev/null 2>&1

exec idf.py "$@"
