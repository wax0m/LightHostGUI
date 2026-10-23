# Changelog

All notable changes to LightHost GUI. Format based on [Keep a Changelog](https://keepachangelog.com/).

## 1.1.0 — 2026-10-23

### Added
- **MIX knob (dry/wet) on every plugin card**, next to the plugin's own parameters.
- **Canvas zoom (30–200 %) and pan**, a floating ADD button, wires that can be dropped straight onto a card,
  and selectable cables (click to select; double-click, the x badge, Delete or right-click removes).
- **Missing-plugin pass-through**: a plugin that can't be loaded no longer cuts the chain. Audio flows through
  its card and its saved settings are kept. Double-click the card (or use the tray item) to retry loading it.
- **Mono Input option** (tray): sums left and right onto both channels, for one-sided sources such as a mic.
- Knobs: double-click resets to default; the value is shown while dragging; tooltips show the full text of
  cut-off card labels and values.
- Optional **ASIO** support when building from source with an ASIO SDK (`-DASIO_SDK_DIR`).
- A soft limiter on the final output.

### Changed
- Deleting a card now reconnects its neighbours instead of flattening the graph into a chain; this also works
  on branched graphs, and the tray's add-plugin list is available on them too.
- A new card is placed between the last card and OUT.
- Canvas positions are stored as absolute coordinates (see *Upgrading from 1.0*).
- The window title is "LightHost GUI". The AGPLv3 text now ships with the app.

### Fixed
- Plugins that start in mono (e.g. MeldaProduction MAutoPitch) no longer lose a channel; hosted plugins are
  switched to stereo buses where they support it.
- A settings file that can't be read is copied aside (`…-unreadable-<date>`) instead of being overwritten.
- Out-of-range values in saved settings are clamped instead of trusted.
- Plugin editors are closed and audio stopped before the graph is torn down (quit, preset switch).
- The canvas no longer shrinks below the visible area after moving or deleting a card; the zoom control keeps
  a fixed distance from the corner.

### Upgrading from 1.0
Your settings are migrated once, on first start, to a new canvas format. Before the first save, a copy of the
old file is kept next to it in `%APPDATA%\Light Host\` as `Light Host.settings-v1-<date>-<time>`. Version 1.0
cannot read the migrated file: to go back, quit LightHost and restore that copy over `Light Host.settings`.

### Known limitations
- VST2 plugins are not supported; only VST3 (the VST2 SDK is no longer available from Steinberg). VST2 plugins
  in a saved graph keep their settings and pass audio through unchanged.
- Windows build only; macOS/Linux build from source.
- The Windows build is not code-signed; Windows SmartScreen may warn on first start.

## 1.0.0 — 2026-07-10
First release of the fork: node-graph GUI on top of the original Light Host, tray operation preserved.
