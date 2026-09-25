# Decompilation conventions (ARM sound driver)

Goal: a **logical, compile-able C reconstruction** of every reachable routine in the Soul Calibur
AICA ARM7DI driver (`bin/manatee_arm.bin`, loaded at sound-RAM 0x0). It does not need to match
byte-for-byte. It must be faithful in behaviour, and honest about uncertainty.

## Inputs

- `bin/manatee_arm.bin` — the raw image (base address 0). `tools/extract_sdrv.py` regenerates it.
- `out/ghidra/listing.txt` — full annotated listing (may have stale names; addresses are authoritative).
- `out/ghidra/functions/<addr>_<name>.txt` — per-function disassembly + Ghidra decompilation.
- Live Ghidra (GUI, GhidraMCP plugin on `http://127.0.0.1:8089`), e.g.
  `curl -s "http://127.0.0.1:8089/decompile_function?address=0x9d4"`,
  `.../disassemble_function?address=0x9d4`, `.../get_xrefs_to?address=0xc400`.
  Only use read-only endpoints directly; all writes go through the annotation files (below).
- `src/arm/aica.h` (hardware), `src/arm/manatee.h` (shared memory map, structs, prototypes).
- `docs/research/host_protocol.md` — the SH-4 library side of the host command protocol (if present).

## The code is hand-written assembly

- There is no ABI. Routines pass values in arbitrary registers, share scratch state in fixed
  sound-RAM locations and in words embedded in the code image (e.g. `0x1210`-`0x1230`), and
  frequently branch into each other's bodies. Express these as ordinary C parameters, return values,
  `static` variables (for image-embedded scratch/state words) or `manatee.h` overlays (for work RAM).
- Where a routine has several entry points or falls through into another, write separate C
  functions and note it in the comment.
- Computed jumps (`add pc, ...` / `mov pc, rX`) are `switch` statements; continuation pointers stored
  in memory (e.g. the MIDI-in parser state) become an enum state variable.
- Preserve observable behaviour: AICA register writes (values, order where it matters), shared-RAM
  layout the SH-4 can see, and quirks/bugs. Comment on quirks rather than fixing them.

## C style

- C99, freestanding, types from `types.h` (`u8/u16/u32/s8/...`). Must compile with `make` (clang,
  `--target=arm-none-eabi -march=armv4`). Run `make` before finishing; no warnings in your files if possible.
- One header comment per function, immediately above it:

  ```c
  /*
   * 0x09D4 midi_event_dispatch
   * Confidence: high | medium | low
   * <what it does, in a few lines>
   * Notes: <non-obvious behaviour, quirks, open questions; say what you are unsure of>
   */
  ```

  Confidence means how sure you are that the C reproduces the original's behaviour:
  **high** = every path checked against the disassembly; **medium** = structure right, some
  details/field meanings inferred; **low** = partial or speculative.
- Name things only when the evidence supports it; otherwise use neutral names with the offset
  (`field_2c`, `sub_7f50`). Put the evidence for a name in the comment when it is not obvious.
- Hardware access through `aica.h` macros/structs. Work-RAM through `manatee.h` overlays.

## Shared header `src/arm/manatee.h`

- Add struct fields as you identify them, replacing the `raw[]`/`_NN` padding, and keep the
  `SIZE_CHECK`/`OFFSET_CHECK`s passing (add `OFFSET_CHECK`s for fields you add).
- Several people edit this file concurrently. Always re-Read it right before editing and make small,
  targeted `Edit`s. Never rewrite the whole file, never reorder or delete fields someone else added
  without a strong reason (mention it in your report). Put prototypes under your file's section.

## Ghidra annotations

- Each work area owns one file `tools/ghidra/annotations/NN_<area>.json`; do not edit other areas'
  files. Schema is documented at the top of `tools/ghidra/ApplyManatee.java`:
  `functions` (addr, name, signature, confidence, source, plate), `labels` (addr, name, optional type,
  comment), `comments` (addr, kind eol|pre|post|plate, text), `disasm` (disassemble only),
  `code` (disassemble + create function).
- Keep signatures simple C using built-in types (`void`, `uint`, `int`, `byte`, `uint *`, `byte *`).
- Name every routine you decompile; plate = the short description + key notes; `source` = C file.
- Add EOL/pre comments for non-obvious instructions (bit tricks, magic constants, quirks).
- Apply with `tools/ghidra/apply_live.sh` (idempotent, applies all files). Check its output for errors.
  Do **not** run `tools/ghidra/export_live.sh` (the coordinator refreshes `out/ghidra`).

### Types, register parameters and locals (added during the first pass)

- `apply_live.sh` regenerates `build/ghidra_types.h` from `manatee.h` (`tools/gen_ghidra_types.py`)
  and imports every struct/typedef/enum into Ghidra's `/manatee` category. Never declare structs in
  JSON; keep `manatee.h` right and use the names in signatures and label types (`"Voice[48]"`).
- Hand-written asm passes values in arbitrary registers. Pin them per function with
  `"params":[{"name":"ev","type":"uint","reg":"r1"}]` and `"ret":{"type":"uint","reg":"r1"}`
  (custom storage; the `params` list replaces the signature's parameters).
- Rename/retype decompiler locals with `"locals":{"iVar1|iVar2":"i","puVar3":{"name":"ch","type":"MidiChannel *"}}`.
  Keys are the decompiler's current names; `a|b` alternatives help because numbering changes when
  types change. Already-renamed variables are skipped silently.
- Prefer **stable storage keys** for locals: `out/ghidra/functions/*.txt` lists each local as
  `name type key`, e.g. `@r0:0x880` (register + first use) — use `"@r0:0x880":"eg"`. Parameters are
  never renamed through `locals` (that used to delete pinned parameters).
