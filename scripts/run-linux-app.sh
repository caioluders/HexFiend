#!/usr/bin/env bash
set -euo pipefail

build_dir="${HEXFIEND_BUILD_DIR:-/tmp/hexfiend-linux-app}"
gnustep_root="${HEXFIEND_GNUSTEP_ROOT:-/tmp/hexfiend-gnustep/root}"
app="$build_dir/ui/linux/HexFiendLinux"

if [[ ! -x "$app" ]]; then
    printf 'HexFiendLinux was not found at %s\n' "$app" >&2
    printf 'Build it first with the Linux CMake commands in README.md.\n' >&2
    exit 1
fi

if [[ ! -d "$gnustep_root/usr/lib" ]]; then
    printf 'GNUstep runtime directory was not found at %s/usr/lib\n' "$gnustep_root" >&2
    printf 'Run scripts/stage-arch-gnustep.sh or set HEXFIEND_GNUSTEP_ROOT.\n' >&2
    exit 1
fi

export LD_LIBRARY_PATH="$gnustep_root/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$app" "$@"
