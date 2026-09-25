# SH-4 -> AICA host protocol of the Sega `sd` library (sd 1.00.18) in Soul Calibur (USA)

Research notes. They cover the SH-4 side only (library code linked into `1ST_READ.BIN`, base 0x8C010000).
Confidence labels:

- **[C] confirmed**: read directly from the game disassembly, or from the byte-identical R9 library object.
- **[S] strong inference**: follows from the code, but the ARM-side behaviour it implies was not checked here.
- **[T] tentative**: a guess or an interpretation.

## 0. Sources

| Source | Use |
| --- | --- |
| `~/p/hl-dc-soul-calibur/artifacts/wave2-sound/screen/disasm.txt` | Annotated disassembly of the 112 sd functions linked into the game. All `0x8C2xxxxx` addresses below come from it. |
| `.../research/sdk-nonleaf/evidence/headers/katana-r9/sg_sd.h` | Matching SDK header (`SDD_LIB_STRING "1.00.18"`). |
| `.../artifacts/sdk-library-extract/r9/out/Libraries/Gnu/libsg_sd.a` | Katana R9 GNU archive of the same build (`sd Ver 1.00.18 Build:Apr 05 1999 19:59:31`). It has one object per API, with symbols and relocations. The prior wave-2 research showed that 106 of the 112 game bodies are byte-identical to their members. I unpacked it and disassembled every member with a small SH disassembler in `/tmp/sdlib` (scratch, not kept). This gives the command codes of APIs that the game does **not** link. Those rows are marked "lib-only". |
| `~/p/ghidra-soulcalibur/bin/manatee_arm.bin` / `sdrv_container.bin` | Driver image and SDRV container, used to check the header and the 0xC0 table values. |

Global symbols in the game (from R9 relocations, confirmed by use):

| Address | Symbol | Meaning |
| --- | --- | --- |
| 0x8C246DC8 | `_sdgWrkPtr` | Pointer to the library work area (`W` below). It is 0 until `sdLibInit`. |
| 0x8C246DCC | `_sdgExistSndDrv` | Set to 1 by `sdDrvInit` at 0x8C21CC6C. `sdSysServer` does nothing unless it is 1. |
| 0x8C246DC0 / DC4 | `_sdgErrApi` / `_sdgErrResult` | Written by `sdSysErrTrap` (API entry address and error code). |
| 0x8C246DB8 | (DSG of `sdSndMemClear`) | The word `"SEGA"`. It is the fill pattern for the sound-RAM clear (section 5). |
| 0x8C246B00 / 0x8C246B04 | DSG of `sdPstmSetPan` / `sdPstmSetVol` | Channel-mask tables `{0x20, 0x10, 0x00, 0x30}`, indexed by `target_ch & 3`. |
| 0x8C37A040 | `W` (aligned `_sdgDefaultWrkImg`) | `sdSysWrkInit` rounds 0x8C37A047 down to a 32-byte boundary (0x8C22BAD2..DC). `sdLibInit` ignores its arguments and calls `sdSysWrkInit(0)`, so the default buffers are always used. |
| 0x8C37AFC0 | primary host-command buffer (0x200 bytes, 32-byte aligned) | |
| 0x8C37B1D4 | second host-command list (256 x 0x1C bytes) | |
| 0x8C37A5B4 / 0x8C37ADB4 | memblk handle pool (64) / memblk DMA queue (64) | |

## 1. Locating the driver work areas

### 1.1 Addresses and conventions

- **[C]** Every sound-RAM address in the library is an ARM-side offset (0..0x1FFFFF). The accessors reject offsets >= 0x200000 and add 0xA0800000. They go through `syG2Read` / `syG2Write` (G2-FIFO safe):
  - `sdSndMemReadUint32` 0x8C22A8A4
  - `sdSndMemWriteUint32` 0x8C22A8DC
  - `sdSndMemReadSint32` 0x8C23EDF4
  - `sdSndMemRead(U|S)int8` 0x8C23EE68 / 0x8C23EE2C
  - `sdSndMemWriteUint8` 0x8C244280

  The byte write is a read-modify-write of the containing aligned dword.
- **[C]** Bulk transfers (`sdMemBlkTransfer`) run in one of two modes, selected by `W+0x1A8` (`sdMemBlkSetTransferMode`; 0 = CPU, 1 = DMA):
  - CPU mode: `syG2Write(src, 0xA0800000 + dst, 4, size/4, 1, 1)` in `sdMoveWrkMemToSndMem` 0x8C2441FC.
  - DMA mode: `syG2DmaSetPrm(ch = W[0x1AC], src, 0xA0800000 + dst, size, ...)` and `syG2DmaTrigger` in `sdMemBlkDmaTransfer` 0x8C23E9B4.
- **[C]** The only AICA register the library touches is **0xA0702C00** (ARM reset register):
  - `sdARM7ResetKeep` 0x8C22A220 reads it, ORs in bit 0 and writes it back.
  - `sdARM7ResetRel` 0x8C22A29C clears bit 0.

  Both run with SR.IMASK = 0xF and then spin 0x100 iterations.
- **[C]** The library **never writes an AICA interrupt register**. No `0xA07028xx` or `0x007028xx` literal exists anywhere in `1ST_READ.BIN` (I scanned the whole image for 32-bit literals and `mov.w` 0x28A0/0x28A4/0x289C/0x28B4/0x28B8/0x28BC). The only other 0xA0702C00 user is at 0x8C22178A. It is outside the sd library, is a VREG/RAM-config read-modify-write (`& 0xFCFF`), and is not an interrupt.

  So the host-command channel is **purely polled**: flag byte plus buffer (section 2).

### 1.2 The 0xC0 table (sdDrvGetInfo, 0x8C22B9A4)

`sdDrvInit` (0x8C21CBE4) calls `sdDrvGetInfo` right after it releases the ARM. `W+0x1CC` (sound-RAM base of the driver) is never written, so it stays 0. `sdDrvGetInfo` then:

| Step | Instruction(s) | Effect | Conf |
| --- | --- | --- | --- |
| info base | 0x8C22B9C4..CC | `W+0x1B0 = W[0x1CC] + 0x20` = **0x20** | C |
| version | 0x8C22B9D2 / 9DE / A0A | `W+0x1D0 = s8 @0x20`, `W+0x1D1 = s8 @0x21`, `W+0x1D2 = s8 @0x22`. If `(0x1D0, 0x1D1) == (1, 0)` it reads @0x23 instead of @0x22. `W+0x1D3 = 0`. | C |
| work table | 0x8C22BA14..A5C | `W+0x1B4..0x1C8 = s32 @ (0x20 + 0xA0 .. 0xB4)`, that is sound RAM **0xC0, 0xC4, 0xC8, 0xCC, 0xD0, 0xD4**. Only the first 6 of the 8 table dwords are read. | C |

This game's driver (`manatee_arm.bin`) holds `01 01 3F 00` at 0x20. The library therefore sees driver version **1.1, build 0x3F**. The version drives the branches in section 3.3. The 0xC0 table maps as follows:

| Table slot (ARM addr) | Value | Cached at | SH-4 use | Conf |
| --- | --- | --- | --- | --- |
| 0xC0 | 0x13F00 | `W+0x1B4` | none (no reader in any R9 member) | C |
| 0xC4 | 0x13600 | `W+0x1B8` | **port status area** (section 4.2) | C |
| 0xC8 | 0x13400 | `W+0x1BC` | **system block**: host-command flag byte, counters, error, FX status (section 4.1) | C |
| 0xCC | 0x14000 | `W+0x1C0` | **bank address table** (section 5) | C |
| 0xD0 | 0x13200 | `W+0x1C4` | **host-command buffer** (section 2) | C |
| 0xD4 | 0x14800 | `W+0x1C8` | none | C |
| 0xD8 | 0x13E00 | not read by 1.00.18 | - | C |
| 0xDC | 0x100 | not read | - | C |

`sdDrvCheckExecute` (lib-only) checks that the eight dwords at 0x00..0x1C each have top byte 0xEA (ARM `B` vectors). If not, it returns `SDE_ERR_SND_DRV_PROBLEM`. **[C]**

## 2. Host-command transport

### 2.1 The three stages

```
API (sdMidiPlay ...) --fills 20-byte static record--> sdSetHostCmd
    --> "2nd host command" list in SH RAM (ring, 256 x 28 B)   [interrupts masked]
sdSysServer (per frame, called from 0x8C03177C)
    --> sd2ndHostCmdSendList: moves <= 32 pending entries into
        the primary buffer (SH RAM, 16-byte records)
    --> sdPrimaryHostCmdFlush: memblk transfer (CPU or G2-DMA)
        primary buffer --> sound RAM 0x13200
    --> sdPrimaryHostCmdFlushCallback (on transfer end):
        sound RAM byte 0x13400 := 1
ARM (every 4 ms): if byte[0x13400] != 0 -> process 0x13200 entries, clear 0x13400
```

### 2.2 Stage 1: the API record and `sdSetHostCmd` (0x8C22A408)

- **[C]** Each API owns a 20-byte object-static record (BSG section; zero-initialised), for example 0x8C33CA38 for `sdMidiPlay`.
  - Record +0x00 is a dword holding the command code. Only its low byte survives.
  - Record +0x04.. are parameter bytes.
  - Fields an API does not write keep their previous (initially zero) value, because the record is static.
- **[C]** `sdSetHostCmd(rec)` runs with IMASK = 0xF (0x8C22A40E..422). It works on the "2nd list" control block at `W+0x158`:

| W offset | Field | Conf |
| --- | --- | --- |
| +0x164 | list base (SH RAM) | C |
| +0x168 | capacity (256; `SDD_2ND_HOST_CMD_BUF_NUM`) | C |
| +0x16C | pending count | C |
| +0x170 | write index | C |
| +0x174 | read index | C |

- If pending == 0x100, it returns `SDE_ERR_HOST_CMD_BUF_NO_ENOUGH` (0x0A000001) (0x8C22A434..440). **[C]**
- Otherwise:
  - `e = base + wi*0x1C`, `wi = (wi+1 == cap) ? 0 : wi+1`.
  - `e[0x14] = 1` (valid).
  - Copy the 20 record bytes to `e[0..0x13]` (`__quick_odd_mvn`, r0 = 0x14).
  - `pending++`.

  2nd-list entry layout (0x1C bytes): `+0x00..0x13` record copy, `+0x14` valid dword, `+0x18` unused. **[C]**

`sdSetHostCmdRsvNum` (0x8C23EF82) only validates its argument (0..0x20). `sdSetHostCmdDelayTime` (0x8C23EFAA) only stores a byte at `port+2`. Neither affects the queue. **[C]**

### 2.3 Stage 2: `sdSysServer` (0x8C21CD10) -> `sd2ndHostCmdSendList` (0x8C22A31C)

1. `sdSysServer` calls `sd2ndHostCmdSendList` only if `W[0x16C] > 0`. It also cross-checks the G2-DMA end and trigger counters. **[C]**
2. Busy checks. Either one returns `SDE_ERR_SND_DRV_BUSY` (0x0B000003) and leaves the queue untouched, to retry next frame. **[C]**
   - `W[0x160] == 1`: a primary flush is still in flight (0x8C22A340).
   - `sdSndMemReadUint8(W[0x1BC])`, i.e. **byte at 0x13400**, is nonzero: the driver has not consumed the previous batch (0x8C22A346..354).
3. Loop from the read index `W[0x174]` (wraps at capacity). For each valid entry, call `sdPrimaryHostCmdWrite(e)`. Stop after 0x20 entries, when pending reaches 0, or after scanning `capacity` slots. **[C]**
4. Then call `sdPrimaryHostCmdFlush`. **[C]**

`sdPrimaryHostCmdWrite` (0x8C23E84C) works on the primary control fields. The **16-byte primary record** it writes is the exact on-wire format.

| W offset | Field |
| --- | --- |
| +0x158 | buffer pointer (0x8C37AFC0) |
| +0x15C | entry count |
| +0x160 | flush-busy flag |
| +0x178 | memblk handle |

| Byte | Content | Source | Conf |
| --- | --- | --- | --- |
| 0 | command code | low byte of record dword 0 (`mov.l @r13,r2; mov.b r2,@r12` at 0x8C23E87E/888) | C |
| 1 | always 0 (buffer is memset to 0 at init, and byte 1 is never written) | 0x8C23E7F8 | C |
| 2..15 | record bytes 4..17 (`memcpy(dst+2, rec+4, 14)`, 0x8C23E884..88C) | | C |

Rule: **record offset k corresponds to wire offset k-2.** All parameter offsets in section 3 are wire offsets.
If count >= 0x20 it returns 0x0A000001. After writing, it sets `e[0x14] = 0` and decrements pending. **[C]**

### 2.4 Stage 3: flush and signal

`sdPrimaryHostCmdFlush` (0x8C23E70C) does nothing if count == 0. Otherwise:

- `W[0x160] = 1`.
- **If count is odd, count++** (0x8C23E722..734). This pads with the next buffer slot, whose byte 0 is already 0, so the transfer is a multiple of 32 bytes. That slot's bytes 2..15 may hold stale data from an earlier command. **[C]**
- `sdMemBlkSetPrm(W[0x178], src = W[0x158], size = count*16, cb = sdPrimaryHostCmdFlushCallback, arg = W+0x158)`.
- `sdMemBlkTransfer(W[0x178], dst = W[0x1C4] = 0x13200)`. **[C]**

`sdPrimaryHostCmdFlushCallback` (0x8C23E774) runs from `sdMemBlkExecuteCallback`. That happens either synchronously (CPU mode) or from `sdMemBlkEndTransferHandller` 0x8C244140 (G2-DMA end). It:

- **writes 1 to the byte at `W[0x1BC]` = 0x13400** (0x8C23E776..782);
- then clears byte 0 of each SH-side primary entry, sets count = 0 and sets `W[0x160] = 0`. **[C]**

Consequences for the ARM side:

- The ARM sees a batch of N 16-byte records at 0x13200.
  - N is even and N <= 32, so the buffer is 0x13200..0x133FF.
  - A trailing pad record has code 0x00.
  - Nothing tells the ARM what N is. **[C]**
- The SH-4 never writes zeros past N in sound RAM. The ARM must therefore either stop at the first code 0x00 and zero the records it consumed, or scan all 32 and zero them. Code 0x00 is also the explicit "null command" (`sdSetNullHostCmd`, section 3). **[S]**
- The ARM must clear byte 0x13400 after consuming the batch. The SH-4 relies on this: it never clears the byte itself, and it refuses to send while the byte is nonzero. **[S]**
- The flag write is a 32-bit read-modify-write of 0x13400..0x13403 (`sdSndMemWriteUint8` -> `ReadUint32` / `WriteUint32`). If the ARM updates bytes 0x13401..0x13403 at the same moment, the update can be lost. **[S]**
- No interrupt is raised. The "SH-4->ARM interrupt" counter at 0x1341C and the 0xFC marker must come from something other than the sd library (or be unused in this game). **[C]** for the library, **[T]** for the rest.

## 3. Host command codes

Notation for the parameter columns:

| Notation | Meaning |
| --- | --- |
| `[n]` | Wire byte n of the 16-byte record at 0x13200 + 16*i. |
| `port` | Port index from the handle (`mov.b @(1,r4)`, 0..7 for MIDI/Shot, 0..3 for PSTM). |
| `+0x80` | The signed `Sint8` argument plus 0x80, stored as a byte. |
| fade | `fade_time` stored as u16 little-endian (low 16 bits of the `Sint32`). |
| pitch/speed | `(value + 0x8000)` stored as u16 little-endian. |
| prio<<3 | Priority shifted left 3 (MIDI/Shot priority is checked to 0..31; PSTM priority must satisfy `(p & 0xF0) == 0`, i.e. 0..15). |

Bytes not listed are whatever the static record last held: 0 unless that API wrote them on an earlier call. **[C]**

### 3.1 Codes linked into the game (all [C]; the address is the instruction that loads the code)

| Code | API | Parameters | Code at |
| --- | --- | --- | --- |
| 0x01 | sdMidiPlay(h, bank, data, prio) | [2]=port [3]=bank [4]=data_num [5]=prio<<3 | 0x8C21B790 |
| 0x02 | sdMidiStop | [2]=port | 0x8C21BB2A |
| 0x03 | sdMidiPause | [2]=port | 0x8C21B71A |
| 0x04 | sdMidiContinue | [2]=port | 0x8C21B582 |
| 0x05 | sdMidiSetVol(h, vol, fade) | [2]=port [3]=vol+0x80 [4..5]=fade | 0x8C21BAAE |
| 0x08 | sdMidiSetPitch(h, pitch, fade) | [2]=port [4..5]=pitch+0x8000 [6..7]=fade (no range check) | 0x8C21BA1E |
| 0x0A | sdMidiSetFxLev(h, lev) | [2]=port [3]=lev+0x80 | 0x8C21B892 |
| 0x11 | sdShotPlay(h, bank, data, prio) | [2]=port [3]=bank [4]=data_num [5]=prio<<3 | 0x8C21C9B0 |
| 0x15 | sdShotSetVol(h, vol, fade) | [2]=port [3]=vol+0x80 [4..5]=fade | 0x8C21CB40 |
| 0x18 | sdShotSetPitch(h, pitch, fade) | [2]=port [4..5]=pitch+0x8000 [6..7]=fade; pitch must be in -0x18FF..0x18FF | 0x8C21CAB6 |
| 0x1A | sdShotSetFxLev(h, lev) | [2]=port [3]=lev+0x80 | 0x8C21CA36 |
| 0x1F | sdShotStopAll | none | 0x8C21CBAC |
| 0x21 | sdPstmPlay(h, pcm_type, freq, prio) | [2]=port \| 0x80 if 2 channels in use \| format (0x00 = 16-bit, 0x10 = 8-bit, 0x30 = ADPCM, i.e. the AICA PCMS value << 4). [3]=base-vol byte (see below). [4..5]=freq u16 LE (raw AICA OCT/FNS value, as in the `SDD_PSTM_FREQ_*` constants). [6]=prio<<3. [7]=ring-buffer (SPSR) bank of ch0. [8]=bank of ch1 (0xFF if unused). | 0x8C21BF78; bytes 0x8C21BF7A..BFC6 |
| 0x22 | sdPstmStop | [2]=port (also resets the SH-side channel counters) | 0x8C21C5D6 |
| 0x25 | sdPstmSetVol(h, ch, vol, fade) | [2]=port \| mask(ch). If ch is 0 or -1: [3]=vol+0x80, [4..5]=fade. If ch is 1 or -1: [7]=vol+0x80, [8..9]=fade. | 0x8C21C4D2 |
| 0x26 | sdPstmSetPan(h, ch, pan, fade) | [2]=port \| mask(ch). ch 0: [3]=pan', [4..5]=fade, [7..9]=0. ch 1: [3..5]=0, [7]=pan', [8..9]=fade. ch -1: only [2] is refreshed. The encoding of pan' is given below. | 0x8C21C31C |
| 0x29 | sdPstmSetFxCh(h, in_ch, base_lev) | [2]=port [3]=fx_in_ch [4]=base_lev+0x80 | 0x8C21C18E |
| 0x2A | sdPstmSetFxLev(h, lev) | [2]=port [3]=lev+0x80 | 0x8C21C206 |
| 0x80 | sdSndStopAll | [2]=[3]=[4]=[5]=1, [6]=[7]=0 | 0x8C21B252 |
| 0x81 | sdSndSetMasterVol(vol) | [2]=(vol<<4) & 0xFF | 0x8C21B16E |
| 0x82 | sdSndSetFxPrg, second command (also sdSndSetFxOut, lib-only) | [2]=fx_out_num | 0x8C21B10C |
| 0x84 | sdSndSetFxPrg(prg, out), first command | [2]=fx_prg_num. The function then sends 0x82 with `out` in the same record. | 0x8C21B0F6 |
| 0x86 | sdMidiSendMes(h, mes, prio) | [4]=mes[0] [5]=mes[1] [6]=mes[2] [7]=mes[3]. [2] and [3] are never written. The handle and priority are only checked or ignored; the port is inside mes[2]. | 0x8C21B812 |
| 0x8A | sdSndSetSpace(space) | [2]=0xFF for `'MONO'`, 0x00 for `'STRO'`; any other value is rejected | 0x8C21B1CC |

Notes on the table:

- **mask(ch)** is `{0:0x20, 1:0x10, -1:0x30}`, taken from the DSG tables at 0x8C246B00 / 0x8C246B04. Bit 5 is the left/ch0 channel and bit 4 the right/ch1 channel. **[C]** values; **[S]** meaning.
- **SDS_MIDI_MES**, as built by `sdMidiSetMes` (0x8C21B8E0), which the game links. **[C]**
  - mes[0] = data2 (0 for status 0xA0 / 0xC0, which carry only one data byte)
  - mes[1] = data1
  - mes[2] = (port << 4) | (status & 0x0F)
  - mes[3] = (status >> 4) & 7
  - A system status (0xF0-0xFF) produces all zeros.
- **PSTM base-vol byte [3]** is `W+0xA8 + port*0x2C + 6`, set by `sdPstmSetBaseVol` (0x8C21C080). **[C]** formula, **[T]** meaning.
  - `vol == 0x7F` gives 0.
  - Any other value gives `(0xFF - 2*vol) & 0xFF`.
  - `sdPstmOpenPort` calls `SetBaseVol(0)`, so the default is 0xFF.
- **PSTM pan' encoding**. **[C]**
  - Mono port: `pan + 0x80`.
  - Stereo port, ch0: `(pan + 0x80)/2 + 0x80`.
  - Stereo port, ch1: `(pan - 0x80)/2 + 0x80`, where `pan - 0x80 == -1` is first forced to -2.
  - Division rounds toward 0.
  - The unrounded per-channel pan is also kept at `W+0xA8 + port*0x2C + 0x1C + ch*4`.

### 3.2 Codes in the same library build but not linked in this game (lib-only, [C] from R9 objects)

| Code | API | Parameters |
| --- | --- | --- |
| 0x00 | sdSetNullHostCmd | code 0; [2..15]=0xEE (record built on the stack) |
| 0x06 | sdMidiSetPan(h, pan, fade) | [2]=port [3]=pan+0x80 [4..5]=fade |
| 0x07 | sdMidiSetSpeed(h, speed, fade) | [2]=port [4..5]=(speed*4)+0x8000 [6..7]=fade |
| 0x0B | sdMidiSetDrctLev(h, lev) | [2]=port [3]=lev+0x80 |
| 0x0E | sdMidiResetPrm(h) / sdMidiResetAllPrm() | [2]=port, or 0xFF for all |
| 0x0F | sdMidiStopAll | none |
| 0x12 | sdShotStop | [2]=port |
| 0x16 | sdShotSetPan(h, pan, fade) | [2]=port [3]=pan+0x80 [4..5]=fade |
| 0x17 | sdShotSetSpeed(h, speed, fade) | [2]=port [4..5]=speed+0x8000 [6..7]=fade; range -0x18FF..0x18FF |
| 0x19 | sdShotSetFxCh(h, in_ch, base_lev) | [2]=port [3]=fx_in_ch [4]=base_lev+0x80 |
| 0x1B | sdShotSetDrctLev | [2]=port [3]=lev+0x80 |
| 0x1E | sdShotResetPrm / sdShotResetAllPrm | [2]=port / 0xFF |
| 0x27 | sdPstmSetSpeed(h, speed, fade) | [2]=port [4..5]=speed+0x8000 [6..7]=fade; range -0x18FF..0x18FF |
| 0x28 | sdPstmSetPitch(h, pitch, fade) | as 0x27 |
| 0x2B | sdPstmSetDrctLev | [2]=port [3]=lev+0x80 |
| 0x2E | sdPstmResetPrm / sdPstmResetAllPrm | [2]=port / 0xFF |
| 0x2F | sdPstmStopAll | none (also resets SH-side PSTM channel state) |
| 0x35 | sdGddaSetVol(l, r) | [2]=l [3]=r, both >= 0. Doubled when driver build >= 0x19 (so doubled for this driver). |
| 0x36 | sdGddaSetPan(l, r) | [2]=l+0x80 [3]=r+0x80. Halved only for driver 1.0/1.1 with build <= 0x11 (not this driver). |
| 0x3E | sdGddaResetPrm | none |
| 0x82 | sdSndSetFxOut(out) | [2]=out |
| 0x83 | sdSndSetFxOutPrm(out, pan, lev) | [2]=out (0..15) [3]=lev+0x80 (lev>>3 first when build < 0x27) [4]=pan+0x80 |
| 0x85 | sdSndClearFxPrg | none |
| 0x88 | sdQsndSetPos(pos[7]) | [2..8]=pos[0..6] |

The numbering is systematic **[S]**:

| Range | Target |
| --- | --- |
| 0x0n | MIDI port |
| 0x1n | One-shot port |
| 0x2n | PCM-stream port |
| 0x3n | GD-DA |
| 0x8n | Global |

Within each port family the low nibble is the same operation:

| Low nibble | Operation |
| --- | --- |
| 1 | Play |
| 2 | Stop |
| 3 | Pause |
| 4 | Continue |
| 5 | Vol |
| 6 | Pan |
| 7 | Speed |
| 8 | Pitch |
| 9 | FxCh |
| A | FxLev |
| B | DrctLev |
| E | ResetPrm |
| F | StopAll |

The library never emits 0x09 (MIDI FxCh), 0x13/0x14 (Shot pause/continue), 0x23/0x24, 0x87, 0x89 or 0x8B+. The driver may still decode them. **[S]**

### 3.3 Driver-version-dependent encodings

`W+0x1D0..0x1D2` holds (major, minor, build), which is (1, 1, 0x3F) for this driver.

| API | Condition | Effect |
| --- | --- | --- |
| sdGddaSetVol | build >= 0x19 | vol * 2 |
| sdGddaSetPan | (1, <=1, <=0x11) | pan halved |
| sdSndSetFxOutPrm | build < 0x27 | lev >> 3 |
| sdDrvGetErr | build < 0x22 | reads the old error location (section 4.1) |

All **[C]** (R9 objects).

## 4. Status and readback structures

### 4.1 System block at `W[0x1BC]` = 0x13400

| Addr | Type | Reader | Meaning | Conf |
| --- | --- | --- | --- | --- |
| 0x13400 | u8 | sd2ndHostCmdSendList (read), FlushCallback (write 1) | host-command batch pending flag | C |
| 0x13402 | u16 | sdDrvGetErr, driver build < 0x22: `(u32 @0x13400) >> 16` | driver error (old location) | C (lib) |
| 0x13414 | u32 | sdDrvGetMidiTimmingCounter | MIDI timing counter | C (lib) |
| 0x13418 | u32 | sdDrvGetExecuteCounter | execute (4 ms tick) counter; matches the ARM-side finding | C (lib) |
| 0x13420 | u32 (low 16 bits used) | sdDrvGetErr, build >= 0x22 (this driver) | driver error bits (`SDD_DRV_ERR_*`: 0x08 bank not downloaded, 0x10 illegal id, 0x20 illegal end id, 0x40 illegal version, 0x80 illegal number) | C (lib) |
| 0x134E0 | s8 | sdSndGetFxPrg | current FX program number | C (lib) |
| 0x134E1 | s8 | sdSndGetFxOut | current FX output number | C (lib) |

`sdDrvGetVer` returns the 4 cached bytes `W+0x1D0..0x1D3`. It does not re-read sound RAM. **[C]** (lib)

### 4.2 Port status area at `W[0x1B8]` = 0x13600

| Port kind | Address | Conf |
| --- | --- | --- |
| MIDI port i (0..7) | 0x13600 + i*0x20 | C (e.g. sdMidiGetVol 0x8C22ADFE..E10) |
| One-shot port i (0..7) | 0x13800 + i*0x20 (`+0x200`) | C (sdShotGet*: `mov.w 0x200`) |
| PCM stream port p (0..3), channel c (0..1) | 0x13A00 + p*0x40 + c*0x20 (`+0x400`) | C (sdPstmGet*: port<<6, ch<<5) |

Every record is 0x20 bytes. Getter function addresses are in the disassembly (0x8C22A9E0..0x8C22B985).

| Off | Type | Getter | Notes | Conf |
| --- | --- | --- | --- | --- |
| +0x00 | u8 | *GetFlg | `SDD_PORT_FLG_*` (1 play, 2 pause, 4/8/0x10/0x20 vol/speed/pan/pitch changing, 0x80 trouble) | C |
| +0x01 | s8 | *GetVol | | C |
| +0x02 | s8 | *GetPan | PSTM stereo readback: ch0 is `(raw + 0x40)*2`; ch1 is `((raw ? raw : -1) - 0x40)*2` | C |
| +0x04 | s32 | *GetSpeed | MIDI: the SH-4 divides by 4 (0x8C22AD2E..36) to form an `Sint16`; Shot/PSTM: truncated to `Sint16` | C |
| +0x08 | s32 | *GetPitch | truncated to `Sint16` | C |
| +0x10 | s8 | *GetFxLev | | C |
| +0x11 | s8 | *GetDrctLev | | C |
| +0x14 | u32 | GetTotalBeatTime (MIDI) / GetTotalSmpFrame | | C |
| +0x18 | s32 | *GetCurAdr | PSTM: the SH-4 converts to bytes (16-bit x2, ADPCM /2, 8-bit x1), 0x8C22AED0..AEF6 | C |
| +0x1C | u32 | *GetErr | `SDD_PORT_ERR_*`; sdShotGetErr keeps only the low byte (`extu.b`, 0x8C22B5D2) | C |

The `Get*Stat` aggregators (`sdMidiGetStat` 0x8C21B5C8, `sdShotGetStat` 0x8C21C838, `sdPstmGetStat` 0x8C21BB70) call the getters and set `m_HostCmdRsvNum` / `m_HostCmdDelayTime` to 0. Nothing else is read from sound RAM. **[C]**

### 4.3 SH-4-only state (no sound-RAM counterpart)

| Location | Content |
| --- | --- |
| W+0x08 + 8i | MIDI port records |
| W+0x48 + 8i | Shot port records |
| W+0x88 + 8i | PSTM port records |

Port record fields (from `sdModuleOpenPort` 0x8C22A91C and sdModuleRefInit): +0 type (1 = MIDI, 2 = Shot, 3 = PSTM), +1 index, +2 delay time, +4 in-use.

PSTM extension at `W+0xA8 + p*0x2C`:

| Off | Content |
| --- | --- |
| +0 | pcm type |
| +4 | channel count at open |
| +5 | use_ch |
| +6 | base-vol byte |
| +7.. | SPSR bank per channel |
| +0xC | per-channel flag |
| +0x14 | ring write offset |
| +0x1C | pan |
| +0x24 | wrap count |

GD-DA shadow values are at W+0x1D6..0x1D9. **[C]**

## 5. Driver and bank download

### 5.1 `sdDrvInit(memblk)` (0x8C21CBE4)

`memblk[+8]` points at an SDRV container (`bin/sdrv_container.bin`).

1. Check the container header. **[C]**
   - Magic `'SDRV'` (0x56524453); otherwise return `SDE_ERR_DATA_ILLEGAL_TYPE`.
   - Byte +4 (major) == 1 and byte +5 (minor) <= 1; otherwise return 0x0B000002.
2. `sdARM7ResetKeep()`: set bit 0 of 0xA0702C00. **[C]**
3. `sdSndMemClear()` (0x8C22A86C) runs ResetKeep again, then `syG2Write(src = 0x8C246DB8 "SEGA", dst = 0xA0800000, 4, 0x80000, 0, 1)`. **[C]** call; **[S]** fill semantics.
   - This fills all 2 MB of sound RAM with the word `"SEGA"`. The 5th argument is 0 here, which suggests a fixed source. The one multi-word copy call, `sdMoveWrkMemToSndMem`, passes 1, 1.
   - The ARM-side "SEGA at 0xF8" may therefore just be this fill pattern until the driver overwrites it.
4. Create a memblk at 0x8C246B08. Call `SetPrm(src = container + 0x20, size = u32 container[+8], cb = 0, arg = 0)`, then `Transfer(dst = 0)`: the image goes to **sound RAM 0**. **[C]**
   - `container[+8]` is 0x8320 = the whole container including its 0x20-byte header. The copy therefore runs 0x20 bytes past the end of the image. This is harmless. **[C]**
5. `sdARM7ResetRel()`, then a busy-wait of 0x200000 iterations. **[C]**
6. Set `_sdgExistSndDrv = 1`, call `sdDrvGetInfo` (section 1.2), then call the memblk callback. **[C]**
7. **No handshake**: the library does not poll 0xF8, 0xFC or any ready flag. **[C]**

### 5.2 Bank table at `W[0x1C0]` = 0x14000

`sdSndMemAllocateBank(type, num, addr, size)` (0x8C22A6D4, called by `sdMultiUnitDownload`) writes `s32 addr` then `s32 size` into the table. It uses `sdSndMemWriteSint32` at 0x8C22A81E..82C. It keeps an identical SH-side mirror at `W+0x1E4 + (same offset)`. That mirror is what `sdSndMemGetBankStat` (0x8C21B434) and `sdBankDownload` read. **[C]**

| Type | Sound-RAM entry | Allowed num | SH mirror |
| --- | --- | --- | --- |
| SMSB (MIDI sequence bank) | 0x14000 + num*8 | 0..15 | W+0x1E4 |
| SMPB (MIDI program bank) | 0x14080 + num*8 | 0..15 | W+0x264 |
| SOSB (one-shot bank) | 0x14100 + num*8 | 0..15 | W+0x2E4 |
| SPSR (PCM-stream ring buffer) | 0x14180 + num*8 | 0..15 | W+0x364 |
| SFPB (FX/DSP program bank) | 0x14200 | 0 | W+0x3E4 |
| SFOB (FX output bank) | 0x14280 | 0 | W+0x464 |
| SFPW (FX/DSP work, i.e. the DSP ring buffer) | 0x14288 (addr), 0x1428C (size) | 0 | W+0x46C |

Other type codes are rejected with `SDE_ERR_DATA_ILLEGAL_TYPE`: SDRV, SMLT, SMSD, SOSD and SPSD. A bad number is rejected with `SDE_ERR_BANK_ILLEGAL_NUM`. **[C]**

### 5.3 `sdMultiUnitDownload(memblk)` (0x8C21B320)

1. Check the header magic `'SMLT'`. **[C]**
2. Read `count = u32 hdr[+8]`. Entries are 0x20 bytes each and start at `hdr + 0x20`. **[C]**
   - Old files (`hdr[+4] < 1`) start at `hdr + 0x10` instead and get error code 0x09000002. The entries are still processed. **[T]** as to intent.
3. For each entry: **[C]**
   - Entry fields: `+0` type fourcc, `+4` bank num, `+8` sound-RAM address, `+0xC` size, `+0x10` file offset of data, `+0x14` data size.
   - If `size >= 0`: call AllocateBank.
   - If `data size > 0`: `SetPrm(tmp, hdr + off, data size, SYNC(-1))`, then `sdBankDownload(tmp, type, num)`. That call reads the address back from the SH mirror and runs `sdMemBlkTransfer(tmp, addr)` **synchronously**.

**No host command is sent for any download.** The driver learns bank locations only by reading the table at 0x14000 when a Play or FX command names a bank or program number. For example, `sdSndSetFxPrg` -> 0x84 / 0x82 relies on SFPB, SFOB and SFPW. **[C]** for the SH-4 side; **[S]** that the ARM reads the table on use.

### 5.4 PCM-stream data (`sdPstmTransferWaveData`, 0x8C21C620)

This path is also command-free. **[C]**

1. It DMAs the memblk to `SPSR[bank].addr + write_offset`.
2. The ring size is clamped:

   | Format | Clamp |
   | --- | --- |
   | ADPCM | size > 0x7F00 is clamped to 0x7000 |
   | 8-bit | size > 0xFF00 is clamped to 0xF000 |
   | 16-bit | no clamp |

3. On a channel's first transfer it copies the u32 at `ring[0]` to `ring[size]`, so the last sample wraps around.
4. It advances the write offset, wrapping it and incrementing the wrap count.

`sdPstmIsTransferWaveData` compares the free space with the ARM play position (+0x18 of the channel's status record). Its caps are 0x1F000 / 0xF000 / 0x7000 for 16-bit / 8-bit / ADPCM. **[C]**

## 6. Open questions

1. How does the ARM decide how many records to process: stop at code 0x00, or always read 32? And does it zero the consumed records? The SH-4 never clears stale records in sound RAM (section 2.4). Check the ARM command loop and the clearing of 0x13400.
2. What raises the ARM "SH-4 interrupt" (0x1341C counter, 0xFC = 0xFFFFFFFF)? No code in `1ST_READ.BIN` references an AICA interrupt register. Possibilities: an unused path, other middleware (ADX/Sofdec `*_mw` wrappers only call the plain sd APIs), or a timer/DMA interrupt counted by the same handler.
3. What are 0x13F00 (table slot 0xC0), 0x14800 (0xD4) and 0x13E00 (0xD8) for? The SH-4 library caches the first two and never uses them, so they are ARM-internal.
4. The meaning of the 0x80 SndStopAll flags [2..5] (probably MIDI / Shot / PSTM / GD-DA stop enables) and [6..7]. Also the meaning of MasterVol's `vol<<4` byte (AICA MVOL is 4 bits, so the ARM probably uses `>>4`). **[T]**
5. The PSTM base-vol byte `0xFF - 2*vol` (0x7F -> 0) looks like an attenuation value (TL-like). Confirm on the ARM.
6. The PstmSetPan / PstmSetVol channel mask (0x20 = ch0, 0x10 = ch1) and the stereo pan remapping should be matched against the ARM PSTM handler.
7. `sdSndMemClear`'s `syG2Write` argument semantics (5th = source increment?). The SH-4 ABI of `syG2Write` was inferred from its call sites, not from its body.
8. Is the RMW race on 0x13400..0x13403 real? Does the ARM write 0x13401..0x13403?
