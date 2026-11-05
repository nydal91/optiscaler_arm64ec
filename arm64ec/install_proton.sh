#!/usr/bin/env bash
# Non-interactive OptiScaler ARM64EC installer for Proton/Wine on ARM64 Linux.
#
# Usage:
#   bash install_proton.sh [GAME_DIR] [PROXY_NAME]
#
# GAME_DIR   Directory containing the real game executable. Defaults to the
#            current working directory.
# PROXY_NAME Proxy filename, e.g. dxgi.dll (default), winmm.dll, version.dll.
#
# This script only copies files; it does not modify the Wine prefix. Set the
# printed WINEDLLOVERRIDES value in the Steam launch options or the environment.

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
game_dir="${1:-$PWD}"
proxy="${2:-dxgi.dll}"

case "$proxy" in
    *.dll) ;;
    *) echo "ERROR: proxy name must end in .dll (got '$proxy')" >&2; exit 2 ;;
esac

if [ ! -d "$game_dir" ]; then
    echo "ERROR: game directory does not exist: $game_dir" >&2
    exit 2
fi
if [ ! -f "$script_dir/OptiScaler.dll" ]; then
    echo "ERROR: OptiScaler.dll not found next to this script ($script_dir)" >&2
    exit 2
fi

# Refuse to install over an existing non-OptiScaler proxy.
if [ -e "$game_dir/$proxy" ]; then
    if ! grep -aq "OptiScaler" "$game_dir/$proxy" 2>/dev/null; then
        echo "ERROR: $game_dir/$proxy already exists and does not look like OptiScaler." >&2
        echo "       Remove or back it up first, or choose another proxy name." >&2
        exit 3
    fi
    echo "Replacing existing OptiScaler proxy: $game_dir/$proxy"
fi

install -d "$game_dir/OptiScaler"
cp -f "$script_dir/OptiScaler.ini" "$game_dir/"
cp -f "$script_dir/README-ARM64EC.md" "$game_dir/" 2>/dev/null || true
if [ -d "$script_dir/OptiScaler" ]; then
    cp -a "$script_dir/OptiScaler/." "$game_dir/OptiScaler/"
fi
if [ -d "$script_dir/Licenses" ]; then
    cp -a "$script_dir/Licenses" "$game_dir/"
fi
cp -f "$script_dir/OptiScaler.dll" "$game_dir/$proxy"

dll_base="${proxy%.dll}"
echo
echo "Installed OptiScaler ARM64EC as $game_dir/$proxy"
echo "Launch the game with:"
echo "    WINEDLLOVERRIDES=\"$dll_base=n,b\" %command%"
echo
echo "Press Insert in-game to open the overlay. Log: $game_dir/OptiScaler.log"
