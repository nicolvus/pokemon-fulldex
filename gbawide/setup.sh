#!/usr/bin/env bash
# Fetches mGBA (the version PokeDaisy's EXPAND is built with) into gbawide/deps/mgba.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MGBA_URL=https://github.com/mgba-emu/mgba.git
MGBA_COMMIT=26b7884bc25a5933960f3cdcd98bac1ae14d42e2 # 0.10.5, as PokeDaisy pins it
mkdir -p "$HERE/deps"
if [ ! -d "$HERE/deps/mgba/.git" ]; then
    git init -q "$HERE/deps/mgba"
    git -C "$HERE/deps/mgba" remote add origin "$MGBA_URL"
fi
git -C "$HERE/deps/mgba" fetch -q --depth 1 origin "$MGBA_COMMIT"
git -C "$HERE/deps/mgba" checkout -q --force FETCH_HEAD
echo "mGBA $(git -C "$HERE/deps/mgba" log -1 --format=%h) in $HERE/deps/mgba"
