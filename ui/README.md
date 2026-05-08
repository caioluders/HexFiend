# UI

This directory is the destination for the Linux-native application layer.

The existing AppKit application under `app/` is the UI reference for this branch.
Linux UI work should preserve the same document-window structure, menus,
representers, operation banners, inspector/status areas, and workflows unless a
Linux platform constraint makes a direct match impossible.

The first Linux target is `ui/linux`, an ImGui + SDL2/OpenGL application that
links against `HexFiendCore`. It intentionally mirrors the macOS document window:
Find/Replace banner, line numbers, hex and text representers, vertical scroller,
Data Inspector, status bar, and the same high-level menu groupings.
