// Pre-analysis setup for the Soul Calibur Manatee ARM7DI sound driver image.
// Memory map is the AICA ARM-side view:
//   0x00000000-0x001FFFFF  sound RAM (driver image at 0, rest is work/bank RAM)
//   0x00800000-0x0080FFFF  AICA registers (channels, common, DSP)
// @category Manatee
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.SymbolTable;
import ghidra.program.model.data.*;

public class SetupManatee extends GhidraScript {
    private void label(long a, String name, String cmt) throws Exception {
        Address addr = toAddr(a);
        currentProgram.getSymbolTable().createLabel(addr, name, SourceType.USER_DEFINED);
        if (cmt != null) setEOLComment(addr, cmt);
    }

    @Override
    public void run() throws Exception {
        Memory mem = currentProgram.getMemory();
        MemoryBlock code = mem.getBlock(toAddr(0));
        code.setName("drv_image");
        code.setWrite(true);
        code.setExecute(true);
        long end = code.getEnd().getOffset() + 1;

        MemoryBlock ram = mem.createUninitializedBlock("sndram", toAddr(end), 0x200000 - end, false);
        ram.setRead(true); ram.setWrite(true); ram.setExecute(false);

        MemoryBlock regs = mem.createUninitializedBlock("aica_regs", toAddr(0x00800000), 0x10000, false);
        regs.setRead(true); regs.setWrite(true); regs.setVolatile(true);

        // Exception vectors (ARMv3 layout; 0x14 is the 26-bit address exception slot).
        String[] vec = {"vec_reset", "vec_undef", "vec_swi", "vec_pabort",
                        "vec_dabort", "vec_addrexc", "vec_irq", "vec_fiq"};
        for (int i = 0; i < 8; i++) {
            Address a = toAddr(i * 4);
            label(i * 4, vec[i], null);
            disassemble(a);
            addEntryPoint(a);
        }

        // Per-channel register block (64 channels, stride 0x80).
        StructureDataType ch = new StructureDataType("AicaChannel", 0);
        String[] chn = {"sa_hi_ctl", "sa_lo", "lsa", "lea", "env_ad", "env_dr", "pitch", "lfo",
                        "dsp_send", "direct", "tl_q", "flv0", "flv1", "flv2", "flv3", "flv4",
                        "fenv_a", "fenv_r"};
        String[] chc = {"KYONEX15 KYONB14 SSCTL10 LPCTL9 PCMS8:7 SA22:16", "SA15:0", "LSA", "LEA",
                        "D2R15:11 D1R10:6 AR4:0", "LPSLNK14 KRS13:10 DL9:5 RR4:0",
                        "OCT14:11 FNS10:0", "LFORE15 LFOF14:10 PLFOWS9:8 PLFOS7:5 ALFOWS4:3 ALFOS2:0",
                        "IMXL7:4 ISEL3:0", "DISDL11:8 DIPAN4:0", "TL15:8 VOFF6 LPOFF5 Q4:0",
                        "FLV0", "FLV1", "FLV2", "FLV3", "FLV4", "FAR12:8 FD1R4:0", "FD2R12:8 FRR4:0"};
        for (int i = 0; i < chn.length; i++) ch.add(DWordDataType.dataType, chn[i], chc[i]);
        ch.add(new ArrayDataType(DWordDataType.dataType, (0x80 - chn.length * 4) / 4, 4), "pad", null);
        DataType chArr = new ArrayDataType(ch, 64, 0x80);
        currentProgram.getDataTypeManager().addDataType(ch, null);
        label(0x00800000, "AICA_CH", "64 x AicaChannel");
        createData(toAddr(0x00800000), chArr);

        label(0x00802000, "AICA_CDDA_OUT", "CDDA L/R EFSDL/EFPAN (2 x 0x44 regs)");
        label(0x00802800, "AICA_MVOL", "MONO15 MEM8MB9 DAC18B8 VER7:4 MVOL3:0");
        label(0x00802804, "AICA_RINGBUF", "TESTB0 RBL14:13 RBP12:0");
        label(0x00802808, "AICA_MIDI_IN", "MOFUL12 MOEMP11 MIOVF10 MIFUL9 MIEMP8 MIBUF7:0");
        label(0x0080280C, "AICA_MSLC", "AFSEL14 MSLC13:8 MOBUF7:0");
        label(0x00802810, "AICA_MON_EG", "LP15 SGC14:13 EG12:0 (monitor of MSLC channel)");
        label(0x00802814, "AICA_MON_CA", "CA (monitor of MSLC channel)");
        label(0x00802880, "AICA_DMA_HI", "MRWINH3:0 / DMEA22:16");
        label(0x00802884, "AICA_DMA_ADDR", "DMEA15:2 TSCD2:0");
        label(0x00802888, "AICA_DMA_CTL", "DGATE15 DRGA14:2");
        label(0x0080288C, "AICA_DMA_LEN", "DDIR15 DLG14:2 DEXE0");
        label(0x00802890, "AICA_TIMA", "TACTL10:8 TIMA7:0");
        label(0x00802894, "AICA_TIMB", "TBCTL10:8 TIMB7:0");
        label(0x00802898, "AICA_TIMC", "TCCTL10:8 TIMC7:0");
        label(0x0080289C, "AICA_SCIEB", "ARM int enable");
        label(0x008028A0, "AICA_SCIPD", "ARM int pending (write bit5 = SH-4->ARM int)");
        label(0x008028A4, "AICA_SCIRE", "ARM int reset (ack)");
        label(0x008028A8, "AICA_SCILV0", "ARM int level bit0");
        label(0x008028AC, "AICA_SCILV1", "ARM int level bit1");
        label(0x008028B0, "AICA_SCILV2", "ARM int level bit2");
        label(0x008028B4, "AICA_MCIEB", "SH-4 int enable");
        label(0x008028B8, "AICA_MCIPD", "SH-4 int pending (write bit5 = ARM->SH-4 int)");
        label(0x008028BC, "AICA_MCIRE", "SH-4 int reset");
        label(0x00802C00, "AICA_ARMRST", "VREG9:8 ARMRST0");
        label(0x00802D00, "AICA_INTREQ", "L7:0 current FIQ level (read)");
        label(0x00802D04, "AICA_INTCLR", "M0: write 1 to clear FIQ");
        label(0x00803000, "AICA_DSP_COEF", "128 x 13-bit coefficients");
        label(0x00803200, "AICA_DSP_MADRS", "64 x 16-bit memory addresses");
        label(0x00803400, "AICA_DSP_MPRO", "128 steps x 4 words microprogram");
        label(0x00804000, "AICA_DSP_TEMP", "128 x 24-bit temp");
        label(0x00804400, "AICA_DSP_MEMS", "32 x 24-bit");
        label(0x00804500, "AICA_DSP_MIXS", "16 x 20-bit");
        label(0x00804580, "AICA_DSP_EFREG", "16 x 16-bit");
        label(0x008045C0, "AICA_DSP_EXTS", "2 x 16-bit");

        // Driver identification header (between vectors and reset code).
        createData(toAddr(0x20), new ArrayDataType(ByteDataType.dataType, 4, 1));
        label(0x20, "drv_version", "driver version bytes 01 01 3f 00");
        createAsciiString(toAddr(0x24), 4);
        label(0x24, "drv_tag", null);
        createAsciiString(toAddr(0x30), 0x90);
        label(0x30, "drv_credits", null);
        label(0xC0, "drv_layout_table", "sound RAM work-area addresses");
        for (int i = 0; i < 8; i++) createDWord(toAddr(0xC0 + i * 4));
    }
}
