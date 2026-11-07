#!/usr/bin/env bash
# fetch_decomp.sh - check out the Resident Evil PC decompilation at the pinned
# commit into ./decomp and apply this port's patch set (patches/*.patch).
#
# The decomp is GPL-3.0 and is NOT vendored in this repository: it is fetched
# here so the provenance of every game-logic line stays upstream. Re-running
# the script is safe: it resets ./decomp to the pinned commit and re-applies.
#
# Usage: tools/fetch_decomp.sh [--update]   (--update: fetch even if present)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REPO_URL="${RE1_DECOMP_URL:-https://github.com/ecruells/resident-evil-pc-decomp.git}"
PINNED="$(tr -d '[:space:]' < "$ROOT/patches/DECOMP_COMMIT")"
DEST="$ROOT/decomp"

if [ ! -d "$DEST/.git" ]; then
    echo "[fetch] cloning $REPO_URL"
    git clone --filter=blob:none "$REPO_URL" "$DEST"
elif [ "${1:-}" = "--update" ]; then
    git -C "$DEST" fetch origin
fi

if ! git -C "$DEST" cat-file -e "$PINNED^{commit}" 2>/dev/null; then
    git -C "$DEST" fetch origin "$PINNED" || git -C "$DEST" fetch origin
fi

echo "[fetch] resetting to pinned commit $PINNED"
git -C "$DEST" checkout -q --detach "$PINNED"
git -C "$DEST" reset -q --hard "$PINNED"
git -C "$DEST" clean -qfd

for p in "$ROOT"/patches/*.patch; do
    [ -e "$p" ] || continue
    echo "[fetch] applying $(basename "$p")"
    git -C "$DEST" apply --whitespace=nowarn "$p"
done
echo "[fetch] decomp ready at $DEST"
