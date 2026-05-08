<img align="right" width="420" src="docs/linux-screenshot.png?raw=true">

# Hex Fiend for Linux

This fork is focused on a native Linux rewrite of Hex Fiend. The app now builds a portable `HexFiendCore` engine and an ImGui + SDL2/OpenGL frontend named `HexFiendLinux`.

The original Hex Fiend project is a fast macOS hex editor. This branch keeps that engine-driven editing model, but the repository direction here is Linux-first: a C++/ImGui UI, Linux packaging, Linux file/device/process adapters, and a workflow that can be built and tested on a normal Linux workstation.

## Quick Run

From this workspace, run the current local build with:

```sh
scripts/run-linux-app.sh
```

Open a file directly:

```sh
scripts/run-linux-app.sh /path/to/file
```

The built binary is `/tmp/hexfiend-linux-app/ui/linux/HexFiendLinux`. When running it directly, set the staged GNUstep runtime path:

```sh
LD_LIBRARY_PATH=/tmp/hexfiend-gnustep/root/usr/lib /tmp/hexfiend-linux-app/ui/linux/HexFiendLinux
```

Preview builds are available on the [GitHub releases page](https://github.com/caioluders/HexFiend/releases).

## Current Linux App

`HexFiendLinux` currently supports:

- Opening, editing, saving, reverting, and drag-dropping files through `HexFiendCore`.
- Hex and text representers with caret placement, range selection, multi-range selection, line numbers, column headers, and a scroller.
- Insert, overwrite, and read-only modes with undo/redo, cut/copy/paste, paste-as-hex, paste-as-text, and save-as.
- Engine-backed find/replace, jump-to-offset, and status bar selection reporting.
- Endian-aware data inspector values including integers, floats, UTF-8 preview, and LEB128.
- File comparison with insertion-aware diff ranges, diff navigation, range compare, standalone two-file compare, and copyable diff summaries.
- Linux drive opening in read-only mode.
- Process memory region snapshots from `/proc/<pid>/mem`, including process/region filtering and manual snapshot refresh.
- Tcl-backed binary templates for the common Hex Fiend template commands, installed template discovery, includes, sections, and collapsed sections.
- Light/dark theme preferences, recent files, empty-state actions, desktop file, icon, and bundled templates.
- macOS-compatible command-line flags for open file, diff files, and open base64 data, plus `--compare`.

Known gaps are mostly release polish and macOS parity:

- The Linux diff view is functional but simpler than the macOS UI.
- Template support covers the common scripting surface, not every macOS template command.
- Process memory support is snapshot-based, not a live editable memory document.
- More manual GUI testing is still needed across desktop environments, DPI settings, and distributions.

## Build On Linux

The build expects Clang, CMake, SDL2, OpenGL, pkg-config, Tcl, zlib, and a GNUstep Foundation runtime. ImGui is fetched by CMake during configure.

On Arch Linux:

```sh
sudo pacman -S --needed base-devel clang cmake git pkgconf sdl2 libglvnd gnustep-base tcl zlib
```

The repo includes a helper that stages the GNUstep runtime into `/tmp/hexfiend-gnustep/root` without requiring system-wide GNUstep changes:

```sh
scripts/stage-arch-gnustep.sh
```

Configure and build:

```sh
export HEXFIEND_GNUSTEP_ROOT=/tmp/hexfiend-gnustep/root

cmake -S . -B /tmp/hexfiend-linux-app \
  -DCMAKE_OBJC_COMPILER=/usr/bin/clang \
  -DCMAKE_OBJCXX_COMPILER=/usr/bin/clang++ \
  -DBUILD_TESTING=ON

cmake --build /tmp/hexfiend-linux-app -j2
```

Run the tests:

```sh
LD_LIBRARY_PATH="$HEXFIEND_GNUSTEP_ROOT/usr/lib" \
  ctest --test-dir /tmp/hexfiend-linux-app --output-on-failure
```

The test suite covers the portable core, the Linux UI engine bridge, the `HexFiendLinux --self-test` path for command-line handling, preferences, editing, find/replace, diff navigation, templates, and process snapshots, plus a first-frame SDL/OpenGL/ImGui render smoke test when a display is available.

## Install And Run

Install the app, desktop launcher, icon, and bundled templates:

```sh
cmake --install /tmp/hexfiend-linux-app --prefix "$HOME/.local"
```

This installs:

- `HexFiendLinux` into `$HOME/.local/bin`
- the desktop entry into `$HOME/.local/share/applications`
- the icon into `$HOME/.local/share/pixmaps`
- bundled templates into `$HOME/.local/share/hexfiend/templates`

The installed app finds bundled templates relative to its own executable, so custom install prefixes work without setting `HEXFIEND_TEMPLATE_PATH`.

Launch from the build tree:

```sh
scripts/run-linux-app.sh
```

Open a file directly:

```sh
scripts/run-linux-app.sh /path/to/file
```

The built executable is `/tmp/hexfiend-linux-app/ui/linux/HexFiendLinux` by default. Set `HEXFIEND_BUILD_DIR` if you configured CMake into another build directory.

## Repository Layout

- [core/README.md](core/README.md) describes the portable engine.
- [platform/linux/README.md](platform/linux/README.md) describes Linux adapters.
- [ui/README.md](ui/README.md) describes the Linux-native frontend.
- [REWRITE.md](REWRITE.md) records branch rules and migration order.

## Upstream

Hex Fiend originated as a macOS hex editor by ridiculous_fish. This fork is carrying the Linux-native rewrite; upstream macOS documentation and release history remain useful for behavioral reference.
