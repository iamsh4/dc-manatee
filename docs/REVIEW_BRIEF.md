# Reviewer brief (independent verification pass)

You are an independent REVIEWER of the logical C decompilation of the Soul Calibur (Dreamcast) AICA
ARM7DI sound driver. Working directory: the repository root. The code was written by
other agents; your job is to find and fix mistakes, not to trust it.

Read first: docs/CONVENTIONS.md, src/arm/manatee.h, src/arm/aica.h. Format notes: docs/notes/*.md,
docs/research/host_protocol.md.

Sources of truth: the disassembly — `out/ghidra/functions/<addr>_<name>.txt` (disassembly, decompile,
and a "locals" list with stable storage keys; freshly exported) and `out/ghidra/listing.txt` — and the
live Ghidra GUI via read-only GhidraMCP endpoints on http://127.0.0.1:8089
(`decompile_function?address=`, `disassemble_function?address=`, `get_xrefs_to?address=`).
The user is watching Ghidra live.

For EVERY function in your files:

1. Re-derive its behaviour from the ARM disassembly, path by path: conditional execution, flags set by
   earlier instructions, byte vs word accesses, shifts/rotates, register reuse, fallthroughs into other
   routines, literal-pool values. Compare with the C. Fix every behavioural discrepancy in the C (edit in
   place, keep the style). Quirks/bugs of the original must be REPRODUCED and commented, not fixed. If a
   documented "quirk/bug" claim is wrong, correct the comment.
2. Make the header comment's `Confidence:` honest: **high** only if you verified every path; otherwise
   medium/low with a note saying exactly what is uncertain. Add notes for non-obvious behaviour.
3. Ghidra: in the annotation JSON that owns the function (edit only the entries for your functions), make
   sure name / params (registers) / ret / plate are right and update the plate's confidence to match.
   Rename the remaining generic decompiler locals (uVarN, iVarN, puVarN, pXVarN, bVarN, local_*,
   extraout_* where meaningful) using the STABLE storage keys from the locals list in
   `out/ghidra/functions/*.txt`, e.g. `"locals": {"@r4:0x42a0": "port"}`; name keys are fragile.
   Parameters are never renamed via locals. Add EOL comments ("comments" section) for non-obvious
   instructions you had to think about.
4. Apply with `tools/ghidra/apply_live.sh` (idempotent; check for 0 errors; other reviewers run it
   concurrently — ignore errors that come from files that are not yours). Do NOT run export_live.sh.
   To see the effect of renames, query `decompile_function` for that address.
5. `make` and `make link` must pass with no warnings. manatee.h: re-read before editing, small targeted
   edits only (others edit concurrently). Do not edit files outside your group except manatee.h.

Final reply (concise): per function: addr, name, confidence (before -> after), and a one-line description
of every behavioural fix; plus a list of remaining uncertainties. Do not pad.
