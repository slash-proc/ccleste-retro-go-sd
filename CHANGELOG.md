# Changelog

## [v0.0.1]

Initial port of cceleste (Celeste Classic) as a homebrew.

### Added

- Nothing.

### Changed

- Hot game/APU code and blit path linked in ITCM (LMA packed in the GWHB
  payload, copied at boot); assets and cold code stay in RAM_EMU
- Framebuffer and audio mix buffers allocated in DTCM instead of AHB
  (fits the smaller AHB heap on current firmware)

### Fixed

- Nothing.

### Install

- Unzip the release archive onto the SD card root (`homebrew/ccleste.bin`).
- Optional coverflow override: `/covers/homebrew/Celeste.img` (JPEG ≤186×100,
  ≤10 KiB).
