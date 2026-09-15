# Changelog

## [v0.0.4] - 2026-09-16

### Fixed

- Fixed `core_rand` to produce proper dash white-noise sound.

## [v0.0.3] - 2026-09-13

### Changed

- Publish conservative runtime save and savestate support metadata for LFS sizing.

## [v0.0.2]

Joins the GWRG distribution spec: the release now publishes a `manifest.json`,
an offline bundle and the full-size cover beside the binary.

### Added

- `gwrg.json`, declaring `originalSystem: pico8` so a catalogue or web
  installer knows Celeste Classic came from PICO-8 and looks its art up in the
  right library.
- `manifest.json`, the offline bundle and a GitHub Pages `dist/` tree, built by
  the shared `make_manifest.py` / `make_bundle.py` / `build_dist.py`.
- `print-SIDECARS`, `print-RO_BIN` and `print-COVER_FULL` Makefile targets.
  This homebrew installs no sidecar, but `stage_release.py` reads the Makefile
  positionally and a missing target fails the release outright.

### Changed

- `scripts/stage_release.py` replaced with the shared copy, which understands
  `SIDECARS` and publishes `COVER_FULL`.

### Fixed

- Nothing.

### Install

- Unzip the release archive onto the SD card root (`homebrew/Celeste.bin`).
- Optional coverflow override: `/covers/homebrew/Celeste.img` (JPEG <=186x100,
  <=10 KiB).

## [v0.0.1]

Initial port of cceleste (Celeste Classic) as a homebrew.

### Added

- Nothing.

### Changed

- Nothing.

### Fixed

- Dash white noise sound.

### Install

- Unzip the release archive onto the SD card root (`homebrew/ccleste.bin`).
- Optional coverflow override: `/covers/homebrew/Celeste.img` (JPEG ≤186×100,
  ≤10 KiB).
