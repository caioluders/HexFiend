# Core

This directory is the destination for the portable Hex Fiend engine on the `linux-rewrite` branch.

Until extraction is complete, the first portable target is still sourced from `framework/sources/` through `HexFiendCore.h`.

Build ownership now lives here:

- [CMakeLists.txt](/home/g3ol4d0/Desktop/tmp/HexFiend/core/CMakeLists.txt)
- [HexFiendCoreSources.cmake](/home/g3ol4d0/Desktop/tmp/HexFiend/core/HexFiendCoreSources.cmake)
- [MIGRATION.md](/home/g3ol4d0/Desktop/tmp/HexFiend/core/MIGRATION.md)

On Arch Linux without system GNUstep packages, stage the runtime locally first:

```sh
scripts/stage-arch-gnustep.sh
HEXFIEND_GNUSTEP_ROOT=/tmp/hexfiend-gnustep/root cmake -S . -B /tmp/hexfiend-cmake -DCMAKE_OBJC_COMPILER=/usr/bin/clang -DBUILD_TESTING=ON
cmake --build /tmp/hexfiend-cmake -j2
LD_LIBRARY_PATH=/tmp/hexfiend-gnustep/root/usr/lib ctest --test-dir /tmp/hexfiend-cmake --output-on-failure
```
