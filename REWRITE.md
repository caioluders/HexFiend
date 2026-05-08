# Linux Rewrite

This branch is the dedicated Linux rewrite line for Hex Fiend.

## Branch Rules

- `master` remains the macOS/Xcode lineage.
- `linux-rewrite` is free to diverge structurally.
- Reusable engine code should be extracted before UI work is ported.
- AppKit, XIB, launchd, and Mach-specific code are not design constraints for this branch.

## Target Layout

- `core/`
  Portable editing engine, byte arrays, file IO abstractions, search/save logic.
- `platform/`
  OS-specific adapters, starting with Linux.
- `ui/`
  Linux-native application layer.
- `framework/sources/`
  Transitional source location while code is being extracted.

## Migration Order

1. Stabilize the portable `HexFiendCore` surface.
2. Move portable sources behind a clean `core/` target.
3. Introduce Linux platform services for files, process access, and settings.
4. Replace the AppKit document UI with a Linux-native UI.
5. Delete or archive macOS-only pieces from this branch once they no longer serve the rewrite.

## Current Focus

The active branch focus is the Linux-native application in `ui/linux`: keep the ImGui frontend wired to `HexFiendCore`, harden Linux file/device/process workflows, and remove or isolate macOS-only pieces as they stop being useful reference code.
