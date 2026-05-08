# Core

This directory is the destination for the portable Hex Fiend engine on the `linux-rewrite` branch.

Until extraction is complete, the first portable target is still sourced from `framework/sources/` through `HexFiendCore.h`.

Build ownership now lives here:

- [CMakeLists.txt](CMakeLists.txt)
- [HexFiendCoreSources.cmake](HexFiendCoreSources.cmake)
- [MIGRATION.md](MIGRATION.md)

On Arch Linux without system GNUstep packages, stage the runtime locally first:

```sh
scripts/stage-arch-gnustep.sh
HEXFIEND_GNUSTEP_ROOT=/tmp/hexfiend-gnustep/root cmake -S . -B /tmp/hexfiend-cmake -DCMAKE_OBJC_COMPILER=/usr/bin/clang -DBUILD_TESTING=ON
cmake --build /tmp/hexfiend-cmake -j2
LD_LIBRARY_PATH=/tmp/hexfiend-gnustep/root/usr/lib ctest --test-dir /tmp/hexfiend-cmake --output-on-failure
```
