#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# Builds the Monitor Brightness add-on into build/addon with PillTaskbar's add-on tools
# and this folder's addon.conf, then adds settings.schema.json to the bundle.
# Targets as in PillTaskbar's tools/build-addon.sh (default all). PILL_ROOT is a
# PillTaskbar_BloomAddon checkout; without it a sibling folder named
# PillTaskbar_BloomAddon or PillTaskbar is used.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
if [[ -z "${PILL_ROOT:-}" ]]; then
    for d in "$here/../PillTaskbar_BloomAddon" "$here/../PillTaskbar"; do
        if [[ -f "$d/tools/build-addon.sh" ]]; then PILL_ROOT="$d"; break; fi
    done
fi
if [[ -z "${PILL_ROOT:-}" || ! -f "$PILL_ROOT/tools/build-addon.sh" ]]; then
    echo "PillTaskbar_BloomAddon not found: set PILL_ROOT to a checkout of it" >&2
    exit 1
fi
PILL_ROOT="$(cd "$PILL_ROOT" && pwd)"
out="${OUT_DIR:-$here/build/addon}"
echo "tools: $PILL_ROOT"
ADDON_CONF="$here/addon.conf" OUT_DIR="$out" bash "$PILL_ROOT/tools/build-addon.sh" "$@"
if [[ -f "$out/addon.json" ]]; then
    python -I "$here/tools/finish-bundle.py" "$out" "$here/settings.schema.json"
fi
