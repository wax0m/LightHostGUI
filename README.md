# LightHost GUI

A node-graph GUI fork of [Light Host](https://github.com/opencma/LightHost) — a lean,
cross-platform VST3 host that lives in the system tray. LightHost GUI keeps the original's
tray/background operation and adds a full, free-form signal-routing interface with a dark,
professional pro-audio aesthetic (in the lineage of Ableton Live, Bitwig, and Serum).

## Features

- **Free-form node canvas** — every plugin is a card on a scrollable canvas; drag from an output
  port to an input port to wire nodes together. Supports arbitrary routing: serial chains, parallel
  branches, side-chains, and sends. Connections are cycle-validated (no feedback loops); click a
  cable to remove it.
- **Per-node channel strip** — gain, pan, and bypass on every node, each with peak/RMS output
  meters, plus master output meters.
- **Plugin parameter knobs** — each node card can drive the hosted plugin's *own* automatable
  parameters directly, not just the host strip.
- **MIDI routing** — per-node MIDI enable for plugins that accept it.
- **Presets** — named graph snapshots switchable from title-bar tabs or a tray submenu
  (add / rename / delete). Each preset stores the full node graph.
- **VST3 hosting with missing-plugin pass-through** — if a plugin referenced by a saved graph is
  missing, audio passes through that node instead of severing the whole chain.
- **Mono Input toggle** — sum the input to both channels (tray option), for one-sided sources.
- **Native plugin editors** — open each plugin's own editor window.
- **Robust scanning** — multi-instance support and a crash-blacklist plugin scan, preserved from
  upstream Light Host.
- **Low latency** — WASAPI exclusive mode by default; ASIO available when built with an ASIO SDK
  (see below).

## Download

Windows (x64) builds are published as a zip on the GitHub Releases page, with its SHA-256 checksum next to it
(`LightHost-GUI-<version>-win-x64.zip.sha256`). The build is not code-signed. For other platforms, build from source.
See [CHANGELOG.md](CHANGELOG.md) for what changed.

## Build

The first CMake configure downloads JUCE (pinned to 8.0.13) and needs network access.

**Windows** (Visual Studio 2022, x64):
```
cmake -B build
cmake --build build --config Release
```
Binary: `build/LightHost_artefacts/Release/LightHost GUI.exe`

**macOS**:
```
cmake -B build -G Xcode
cmake --build build --config Release
```

**Linux** — install the dev packages first:
```
sudo apt install libasound2-dev libfreetype6-dev libfontconfig1-dev libx11-dev \
    libxrandr-dev libxinerama-dev libxcursor-dev libxext-dev
cmake -B build
cmake --build build --config Release
```

**ASIO** (Windows, optional): pass `-DASIO_SDK_DIR=<path-to-Steinberg-ASIO-SDK>` at configure time.
ASIO is off by default; WASAPI exclusive mode is used otherwise (no SDK required).

## Tests

Tests are off by default. Enable and build the targets you want:

- Headless suite (document / meter / strip / param / routing — no plugins or display):
  ```
  cmake -B build -DLIGHTHOST_BUILD_TESTS=ON
  cmake --build build --config Release --target LightHostTests
  build/LightHostTests_artefacts/Release/LightHostTests
  ```
- Runtime suite (loads real VST3s; skips green if none are installed):
  ```
  cmake -B build -DLIGHTHOST_BUILD_RUNTIME_TESTS=ON
  cmake --build build --config Release --target LightHostRuntimeTests
  build/LightHostRuntimeTests_artefacts/Release/LightHostRuntimeTests
  ```

## Security

Please report security problems privately, not as public issues. See [`SECURITY.md`](SECURITY.md).

## License

LightHost GUI is distributed under the **GNU AGPLv3** (see [`LICENSE`](LICENSE)), because it is built on
JUCE 8, whose open-source licence is AGPLv3.

The original Light Host code is GPL-2.0-or-later and is used here under its "or later" option. The upstream
notice and GPLv2 text are kept for attribution: [`LICENSE-UPSTREAM`](LICENSE-UPSTREAM) and [`gpl.txt`](gpl.txt).

Third-party components:
- JUCE 8: AGPLv3
- VST 3 SDK: MIT (bundled with JUCE 8.0.13)
- ASIO SDK: GPLv3 option, only in builds made with `ASIO_SDK_DIR`
- Plug icon by Bogdan Rosu (flaticon.com): CC BY 3.0

## Credits

A fork of [opencma/LightHost](https://github.com/opencma/LightHost), originally created by
Rolando Islas.
