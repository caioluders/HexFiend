# Core Migration

The first extraction step on `linux-rewrite` is build ownership, not mass file movement.

## Current State

- `core/CMakeLists.txt` owns the `HexFiendCore` target.
- `core/HexFiendCoreSources.cmake` is the source-of-truth manifest for the portable engine.
- Portable implementation files still live under `framework/sources/` while extraction is in progress.

## Deferred Areas

The following categories are intentionally out of the core target for now:

- representers and text views
- pasteboard integration
- AppKit layout and controls
- document-window behavior
- macOS helper-process integration

## Next Extraction Steps

1. Move the core-owned headers from `framework/sources/` into `core/include/HexFiend/`.
2. Move the core-owned implementations into `core/src/`.
3. Replace transitional include-linking in CMake with real `core/include` paths.
4. Introduce Linux platform adapters under `platform/linux/`.
