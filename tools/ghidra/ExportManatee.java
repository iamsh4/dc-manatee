// Export functions, disassembly, decompilation and call graph of the ARM driver.
// Output directory is the first script argument (default: out/ghidra).
// @category Manatee
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class ExportManatee extends GhidraScript {
    // Same key format as ApplyManatee.storageKey (duplicated: scripts are compiled independently).
    static class ApplyManateeKeys {
        static String key(ghidra.program.model.pcode.HighSymbol hs) {
            try {
                ghidra.program.model.listing.VariableStorage st = hs.getStorage();
                if (st == null || st.getVarnodes().length == 0) return "@?";
                if (st.isRegisterStorage() && st.getRegister() != null)
                    return "@" + st.getRegister().getName() + ":" +
                        (hs.getPCAddress() == null ? "?" : "0x" + Long.toHexString(hs.getPCAddress().getOffset()));
                if (st.isStackStorage()) return "@stack:" + st.getStackOffset();
                ghidra.program.model.pcode.Varnode vn = st.getFirstVarnode();
                return "@" + vn.getAddress().getAddressSpace().getName() + ":" + Long.toHexString(vn.getOffset()) +
                    (hs.getPCAddress() == null ? "" : ":0x" + Long.toHexString(hs.getPCAddress().getOffset()));
            } catch (Exception e) { return "@?"; }
        }
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File outDir = new File(args.length > 0 ? args[0] : "out/ghidra");
        File fnDir = new File(outDir, "functions");
        fnDir.mkdirs();

        DecompInterface di = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        opts.grabFromProgram(currentProgram);
        di.setOptions(opts);
        di.openProgram(currentProgram);
        Listing listing = currentProgram.getListing();
        FunctionManager fm = currentProgram.getFunctionManager();

        try (PrintWriter idx = new PrintWriter(new File(outDir, "index.tsv"));
             PrintWriter all = new PrintWriter(new File(outDir, "listing.txt"))) {
            idx.println("entry\tname\tsize\tcallers\tcallees\tsignature");
            for (Function f : fm.getFunctions(true)) {
                Set<String> callers = new TreeSet<>();
                for (Function c : f.getCallingFunctions(monitor)) callers.add(c.getName());
                Set<String> callees = new TreeSet<>();
                for (Function c : f.getCalledFunctions(monitor)) callees.add(c.getName());
                idx.printf("%08x\t%s\t%d\t%s\t%s\t%s%n", f.getEntryPoint().getOffset(), f.getName(),
                    f.getBody().getNumAddresses(), String.join(",", callers), String.join(",", callees),
                    f.getPrototypeString(false, false));

                StringBuilder sb = new StringBuilder();
                sb.append("// ").append(f.getName()).append(" @ ").append(f.getEntryPoint()).append("\n");
                sb.append("// body: ").append(f.getBody()).append("\n");
                sb.append("// callers: ").append(callers).append("\n");
                if (f.getComment() != null) sb.append("/* ").append(f.getComment()).append(" */\n");
                sb.append("\n");
                for (Instruction ins : listing.getInstructions(f.getBody(), true)) {
                    sb.append(fmtIns(ins)).append("\n");
                }
                DecompileResults r = di.decompileFunction(f, 60, monitor);
                if (r != null && r.getHighFunction() != null) {
                    sb.append("\n/* locals (name type key):\n");
                    java.util.Iterator<ghidra.program.model.pcode.HighSymbol> it =
                        r.getHighFunction().getLocalSymbolMap().getSymbols();
                    while (it.hasNext()) {
                        ghidra.program.model.pcode.HighSymbol hs = it.next();
                        sb.append(String.format("     %-16s %-20s %s%s%n", hs.getName(),
                            hs.getDataType() == null ? "?" : hs.getDataType().getName(),
                            ApplyManateeKeys.key(hs), hs.isParameter() ? " (param)" : ""));
                    }
                    sb.append("*/\n");
                }
                sb.append("\n/* ---- decompiled ---- */\n");
                if (r != null && r.decompileCompleted())
                    sb.append(r.getDecompiledFunction().getC());
                else
                    sb.append("/* decompile failed: ").append(r == null ? "null" : r.getErrorMessage()).append(" */\n");
                try (PrintWriter pw = new PrintWriter(new File(fnDir,
                        String.format("%05x_%s.txt", f.getEntryPoint().getOffset(), f.getName())))) {
                    pw.print(sb);
                }
            }
            // Flat listing of the whole image, including data.
            for (CodeUnit cu : listing.getCodeUnits(currentProgram.getMemory().getBlock("drv_image").getAddressRange() == null ? null :
                    new AddressSet(currentProgram.getMemory().getBlock("drv_image").getStart(),
                                   currentProgram.getMemory().getBlock("drv_image").getEnd()), true)) {
                Symbol s = currentProgram.getSymbolTable().getPrimarySymbol(cu.getAddress());
                if (s != null && !s.isDynamic()) all.println(s.getName() + ":");
                Function f = fm.getFunctionAt(cu.getAddress());
                if (f != null && f.getComment() != null) all.println("    ; " + f.getComment().replace("\n", "\n    ; "));
                if (cu instanceof Instruction) all.println(fmtIns((Instruction) cu));
                else all.printf("  %08x  %-10s %s%s%n", cu.getAddress().getOffset(), bytes(cu), cu.toString(),
                        cu.getComment(CodeUnit.EOL_COMMENT) == null ? "" : "  ; " + cu.getComment(CodeUnit.EOL_COMMENT));
            }
        }
        di.dispose();
    }

    private String bytes(CodeUnit cu) {
        try {
            byte[] b = cu.getBytes();
            StringBuilder sb = new StringBuilder();
            for (int i = b.length - 1; i >= 0 && b.length <= 4; i--) sb.append(String.format("%02x", b[i] & 0xff));
            return sb.toString();
        } catch (Exception e) { return ""; }
    }

    private String fmtIns(Instruction ins) {
        StringBuilder sb = new StringBuilder();
        sb.append(String.format("  %08x  %-10s %s", ins.getAddress().getOffset(), bytes(ins), ins.toString()));
        for (Reference r : ins.getReferencesFrom()) {
            if (r.getReferenceType().isFlow() && !r.getReferenceType().isCall()) continue;
            Symbol s = currentProgram.getSymbolTable().getPrimarySymbol(r.getToAddress());
            if (s != null) sb.append("  ; ").append(r.getReferenceType().isCall() ? "call " : "").append(s.getName());
        }
        String c = ins.getComment(CodeUnit.EOL_COMMENT);
        if (c != null) sb.append("  ; ").append(c);
        String pc = ins.getComment(CodeUnit.PRE_COMMENT);
        if (pc != null) sb.insert(0, "    ; " + pc.replace("\n", "\n    ; ") + "\n");
        return sb.toString();
    }
}
