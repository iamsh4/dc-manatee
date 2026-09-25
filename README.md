# dc-manatee

A reverse-engineered, documented reconstruction of **Manatee**, the sound driver Sega shipped with the
Dreamcast (Katana) SDK. Many Dreamcast games load it into the AICA sound chip's ARM7DI processor.
It plays MIDI music and sequenced effects, one-shot sound effects, PCM streams and DSP reverbs on the
AICA's 64 voices.

This repository contains:

| | |
| --- | --- |
| `src/arm/` | A logical, compile-able **C reconstruction of every routine** in the driver: 181 functions, each with a confidence rating and notes on quirks and uncertainties. The original is hand-written ARM assembly; the C preserves its behaviour, including its bugs. |
| `tools/ghidra/` | Everything needed to build the **fully annotated Ghidra project**: 185 named functions with register-accurate signatures, work-RAM structures, AICA register map, jump tables, renamed locals and about 550 comments. |
| `site/` | An illustrated **explainer site** about Dreamcast audio (SH-4 and G2 bus, AICA, ARM7, DSP, interrupts and FIQ) and how the driver works. **Read it online at <https://iamsh4.github.io/dc-manatee/>**, or open `site/index.html`. |
| `docs/` | The same material in Markdown: `HOW_IT_WORKS.md`, plus data-format notes (tone banks, sequences, one-shots/streams, effects, a map of the driver image) and the SH-4 side of the host protocol. |

**No Sega code or data is included.** The Ghidra project and one generated source file (`src/arm/tables.c`)
necessarily contain bytes of the driver, so both are built locally from your own copy (see below).

## Reference build

Everything here was derived from one specific build of the driver:

| | |
| --- | --- |
| Game | *Soul Calibur* (Dreamcast, USA), product T1401N, V1.000 |
| File on disc | `CINIT.DAT`, SHA-256 `b2fd8050b05ab9db3118db9dedfbb6527bb9291452a854191ea7f6d7ccbd68b4` |
| Location | top-level `olnk` entry 2 → nested `olnk` entry 0 (file offset `0x217680`) |
| SDRV container | 0x8320 bytes (32-byte header + image), SHA-256 `12c461d78bcf70e73dccb309979fbf2aa8534112270384fb45b0a0d1b8e85afc` |
| Driver image (`manatee_arm.bin`) | 0x8300 bytes, loaded at AICA sound RAM 0x0, SHA-256 `44f3ab80428e7d5c1f619af6c39ae038028779de232a0cb27ce4911ea15cd9c6` |
| Version | 1.1, build 0x3F (image bytes 0x20-0x22); credit string dated `1999.03.18` |

Other games ship other Manatee builds. The architecture described here applies to them too, but addresses,
command details and bugs may differ, and the tools only accept the reference image.

## Quick start

Requirements: Python 3, clang with the ARM target and `ld.lld` (for `make link`), and Ghidra 12.1.x for the
project. Set `GHIDRA_INSTALL_DIR` if Ghidra is not in `/opt/homebrew/opt/ghidra/libexec`.

```sh
# 1. extract the driver from your copy of the game (checks the SHA-256s above)
python3 tools/extract_sdrv.py /path/to/CINIT.DAT          # -> bin/manatee_arm.bin

# 2. compile-check and link the C reconstruction (generates src/arm/tables.c from the image)
make && make link

# 3. build the annotated Ghidra project
make ghidra                                                # -> ghidra/Manatee.gpr

# 4. read the explainer (also online: https://iamsh4.github.io/dc-manatee/)
open site/index.html
```

`tools/extract_sdrv.py` also accepts an already-extracted `SDRV` container.

## How it fits together

- **C reconstruction.** Each file covers one subsystem:

  | File(s) | Subsystem |
  | --- | --- |
  | `main.c`, `irq.c`, `init.c` | boot, main loop, FIQ timer and MIDI-in interrupt |
  | `event.c`, `voice.c`, `event_ctl.c` | MIDI engine: note on/off, voice allocation, controllers |
  | `sequencer.c`, `port.c` | song players, fades, status export |
  | `host.c`, `host_helpers.c` | command mailbox from the SH-4, one-shot and PCM-stream ports |
  | `fx.c` | DSP effect programs |

  `manatee.h` describes every work-RAM structure, with compile-time offset checks. It is a *logical*
  decompilation: register-passing conventions become C parameters, and image-embedded scratch words
  become statics. It compiles for ARMv4 (`make`) and links as a whole (`make link`), but it is not
  byte-for-byte matching.
- **Ghidra annotations are data.** `tools/ghidra/annotations/*.json` holds all names, signatures (with
  explicit registers), types, comments, jump tables and local-variable names. `SetupManatee.java` sets up
  the memory map at import time and `ApplyManatee.java` applies the annotations idempotently.
  `build_project.sh` runs both.
- **Editing live.** With the [GhidraMCP](https://github.com/bethington/ghidra-mcp) plugin loaded and Ghidra
  started with `GHIDRA_MCP_ALLOW_SCRIPTS=1`:
  - `tools/ghidra/apply_live.sh` applies the JSON to the open project;
  - `export_live.sh` dumps listings and decompilation to `out/ghidra/`;
  - `HarvestLocals.java` snapshots local names you renamed by hand back into JSON.
- **Site:** pages are written in `site/src/` and built with `make site`; `tools/publish_site.sh` publishes
  them to the `gh-pages` branch that GitHub Pages serves.
- **Conventions** for contributors are in `docs/CONVENTIONS.md`.

## Status

- **Coverage:** every reachable routine in the image is decompiled, including the vectors, the FIQ handler,
  the MIDI-in parser, jump-table targets and code reached only through computed jumps. Every byte of the
  image is accounted for in `docs/notes/data_map.md`.
- **Review:** each C file was written, then independently re-derived from the disassembly by a second
  reviewer. 180 of 181 functions are rated **high** confidence. The exception is `ctl_reset_port`, an
  unreferenced alternate entry point.
- **Bugs:** the notable bugs of the original driver (for example voice allocation when every voice has a
  higher priority, and PCM slot stealing) are reproduced and documented in the C, in Ghidra and on the
  site's *Bugs, quirks & confidence* page.

## How this was made

The analysis and everything in this repository were produced with Claude (Anthropic's AI model), working in
Ghidra through the GhidraMCP plugin and in the shell:
- extracting the driver;
- disassembly and annotation;
- the C reconstruction;
- the independent per-file review against the disassembly;
- the documentation and the explainer site.

Hardware descriptions that go beyond what the driver code shows are general Dreamcast/AICA knowledge and
are marked as such. Uncertain findings carry confidence ratings; verify them against the disassembly before
relying on them.

## Credits

The original driver is © SEGA; its credit string attributes it to Sega's Digital Media group. This
repository contains only independently written reconstruction code, annotations and documentation.
