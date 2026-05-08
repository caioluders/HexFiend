#!/usr/bin/env bash
set -euo pipefail

stage_root="${1:-/tmp/hexfiend-gnustep/root}"
package_dir="${2:-/tmp/hexfiend-gnustep/packages}"

mkdir -p "$stage_root" "$package_dir"

download_package() {
    local package="$1"
    local url
    local output="$package_dir/$package.pkg.tar.zst"

    if [[ -f "$output" ]]; then
        return
    fi

    url="$(pacman -Sp "$package" | tail -n 1)"
    if ! curl -L --fail -o "$output" "$url"; then
        rm -f "$output"
        if [[ "$package" != "gcc-objc" ]]; then
            return 1
        fi

        local version encoded_version
        version="$(pacman -Si gcc-objc | awk -F ': ' '$1 == "Version" { print $2; exit }')"
        encoded_version="${version//+/%2B}"
        url="https://archive.archlinux.org/packages/g/gcc-objc/gcc-objc-${encoded_version}-x86_64.pkg.tar.zst"
        curl -L --fail -o "$output" "$url"
    fi
}

for package in gnustep-base gnustep-make libdispatch gcc-objc; do
    download_package "$package"
    bsdtar -xf "$package_dir/$package.pkg.tar.zst" -C "$stage_root"
done

printf 'Staged GNUstep root: %s\n' "$stage_root"
printf 'Configure with: HEXFIEND_GNUSTEP_ROOT=%s cmake -S . -B /tmp/hexfiend-cmake -DCMAKE_OBJC_COMPILER=/usr/bin/clang -DBUILD_TESTING=ON\n' "$stage_root"
