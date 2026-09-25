// Apply repo-tracked annotations (tools/ghidra/annotations/*.json) to the driver program.
// Idempotent: safe to re-run after every edit. Works headless or inside the GUI
// (via GhidraMCP /run_ghidra_script). Optional first arg: annotations directory.
//
// JSON schema (all sections optional):
// {
//   "types":     ["C declarations parsed into the program's data type manager"],
//   "code":      ["0x1234", ...]                       -> disassemble + create function
//   "functions": [{"addr":"0x100","name":"...","signature":"void f(int a)",
//                  "confidence":"high|medium|low","plate":"multi-line text",
//                  // hand-written asm: pin parameters/return to the registers actually used
//                  // (overrides the signature's parameter list; custom storage):
//                  "params":[{"name":"ev","type":"uint","reg":"r1"}], "ret":{"type":"uint","reg":"r0"},
//                  // rename/retype decompiler locals. Keys: current decompiler name(s) "a|b", or the
//                  // stable storage key "@r5:0x1a2c" (register:first-use, listed in out/ghidra/functions);
//                  // parameters are never touched; already-renamed variables are skipped silently:
//                  "locals":{"iVar1":"i", "puVar2":{"name":"ch","type":"MidiChannel *"}},
//                  "noreturn": true}],
//   "labels":    [{"addr":"0x...","name":"...","type":"C type (optional)","comment":"eol"}],
//   "comments":  [{"addr":"0x...","kind":"eol|pre|post|plate","text":"..."}],
//   "flow_overrides": [{"addr":"0xa1c","type":"CALL_RETURN|CALL|BRANCH|RETURN|NONE"}],
//   "jumptables": [{"branch":"0x45c8","table":"0x45cc","count":7}           (table of code pointers)
//                  {"branch":"0x2280","targets":["0x2284","0x2288",...]}]    (explicit targets)
//                  -> computed-jump references + decompiler jump-table override, body re-flowed
//   "body_add":  [{"func":"0x4270","ranges":[["0x42f0","0x42fb"]]}]  (code only reachable through
//                  non-standard flow, e.g. "return to lr+4", added to the function body)
//                  (e.g. jump-table "b" into a separate function = CALL_RETURN, i.e. tail call)
// }
// @category Manatee
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.function.ApplyFunctionSignatureCmd;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.util.cparser.C.CParser;
import ghidra.app.util.parser.FunctionSignatureParser;
import ghidra.app.decompiler.*;
import ghidra.program.model.pcode.*;
import ghidra.program.model.lang.Register;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import com.google.gson.*;
import java.io.*;
import java.nio.file.*;
import java.util.*;

public class ApplyManatee extends GhidraScript {
    private int nFunc, nLabel, nCmt, nErr;

    private Address addr(JsonElement e) {
        return toAddr(Long.decode(e.getAsString()));
    }

    private String str(JsonObject o, String k) {
        return o.has(k) && !o.get(k).isJsonNull() ? o.get(k).getAsString() : null;
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File dir = new File(args.length > 0 ? args[0]
            : new File(getSourceFile().getParentFile().getParentFile().getParentFile().getAbsolutePath(),
                       "tools/ghidra/annotations").getPath());
        File[] files = dir.listFiles((d, n) -> n.endsWith(".json"));
        if (files == null) { printerr("no annotations dir " + dir); return; }
        Arrays.sort(files);
        rootDir = dir.getAbsoluteFile().getParentFile().getParentFile().getParentFile();
        List<JsonObject> docs = new ArrayList<>();
        for (File f : files) {
            try {
                docs.add(JsonParser.parseString(Files.readString(f.toPath())).getAsJsonObject());
            } catch (Exception e) {
                printerr("parse " + f + ": " + e); nErr++;
            }
        }
        // Pass order matters: types, code, functions, labels, comments.
        for (JsonObject d : docs) applyTypes(d);
        for (JsonObject d : docs) applyDisasm(d);
        for (JsonObject d : docs) applyCode(d);
        for (JsonObject d : docs) applyFlowOverrides(d);
        for (JsonObject d : docs) applyFunctions(d);
        fixupOverlappingBodies();
        for (JsonObject d : docs) applyJumpTables(d);
        for (JsonObject d : docs) applyBodyAdd(d);
        for (JsonObject d : docs) applyLabels(d);
        for (JsonObject d : docs) applyComments(d);
        for (JsonObject d : docs) applyProgramInfo(d);
        markConstPools();
        applyLocals();
        println(String.format("ApplyManatee: %d files, %d functions, %d labels, %d comments, %d errors",
            files.length, nFunc, nLabel, nCmt, nErr));
    }

    private File rootDir;

    // Parse C declarations into a scratch manager, move them to /manatee, then resolve into the
    // program with REPLACE so repeated runs update struct layouts in place.
    private void importC(String text, String what) {
        DataTypeManager dtm = currentProgram.getDataTypeManager();
        CategoryPath cat = new CategoryPath("/manatee");
        StandAloneDataTypeManager tmp = new StandAloneDataTypeManager("manatee_tmp");
        int tx = tmp.startTransaction("parse");
        try {
            CParser p = new CParser(tmp, true, null);
            p.parse(text + "\n");
            List<DataType> all = new ArrayList<>();
            tmp.getAllDataTypes(all);
            for (DataType t : all)
                if (t.getCategoryPath().isRoot() && !(t instanceof BuiltInDataType) && !(t instanceof Pointer)
                        && !(t instanceof Array))
                    t.setCategoryPath(cat);
            tmp.endTransaction(tx, true);
            tx = -1;
            int n = 0;
            all.clear();
            tmp.getAllDataTypes(all);
            for (DataType t : all) {
                if (!t.getCategoryPath().equals(cat)) continue;
                if (t instanceof Composite || t instanceof TypeDef || t instanceof ghidra.program.model.data.Enum) {
                    dtm.resolve(t, DataTypeConflictHandler.REPLACE_HANDLER);
                    // drop stale same-named copies outside /manatee (earlier imports, setup script)
                    List<DataType> same = new ArrayList<>();
                    dtm.findDataTypes(t.getName(), same);
                    for (DataType o : same)
                        if (!o.getCategoryPath().equals(cat) && !(o instanceof BuiltInDataType))
                            dtm.remove(o);
                    n++;
                }
            }
            println("types: " + what + ": " + n + " types");
        } catch (Exception ex) {
            printerr("types " + what + ": " + ex.getMessage()); nErr++;
        } finally {
            if (tx >= 0) tmp.endTransaction(tx, false);
            tmp.close();
        }
    }

    private void applyTypes(JsonObject d) {
        if (d.has("type_files")) {
            for (JsonElement e : d.getAsJsonArray("type_files")) {
                File f = new File(rootDir, e.getAsString());
                try {
                    importC(Files.readString(f.toPath()), f.getName());
                } catch (Exception ex) {
                    printerr("type file " + f + ": " + ex.getMessage()); nErr++;
                }
            }
        }
        if (!d.has("types")) return;
        StringBuilder sb = new StringBuilder();
        for (JsonElement e : d.getAsJsonArray("types")) sb.append(e.getAsString()).append("\n");
        importC(sb.toString(), "inline");
    }

    // A function created inside another function's body (e.g. case handlers split out of a big
    // dispatcher) leaves the outer body unchanged. Re-flow every function whose body contains
    // another function's entry so that branches to those entries become tail calls.
    private void fixupOverlappingBodies() {
        FunctionManager fm = currentProgram.getFunctionManager();
        int n = 0;
        for (Function f : fm.getFunctions(true)) {
            boolean overlaps = false;
            for (Function g : fm.getFunctions(f.getBody(), true))
                if (g != f && !g.getEntryPoint().equals(f.getEntryPoint())) { overlaps = true; break; }
            if (!overlaps) continue;
            try {
                if (CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor)) n++;
            } catch (Exception ex) {
                printerr("fixup " + f.getName() + ": " + ex.getMessage()); nErr++;
            }
        }
        println("function bodies re-flowed: " + n);
    }

    private void applyJumpTables(JsonObject d) {
        if (!d.has("jumptables")) return;
        ReferenceManager rm = currentProgram.getReferenceManager();
        for (JsonElement e : d.getAsJsonArray("jumptables")) {
            JsonObject o = e.getAsJsonObject();
            Address br = addr(o.get("branch"));
            try {
                ArrayList<Address> dests = new ArrayList<>();
                if (o.has("targets")) {
                    for (JsonElement t : o.getAsJsonArray("targets")) dests.add(addr(t));
                } else {
                    Address tab = addr(o.get("table"));
                    int n = o.get("count").getAsInt();
                    for (int i = 0; i < n; i++)
                        dests.add(toAddr(currentProgram.getMemory().getInt(tab.add(4 * i)) & 0xFFFFFFFFL));
                }
                for (Address t : dests) {
                    if (getInstructionAt(t) == null) disassemble(t);
                    boolean have = false;
                    for (Reference r : rm.getReferencesFrom(br))
                        if (r.getToAddress().equals(t)) have = true;
                    if (!have) rm.addMemoryReference(br, t, RefType.COMPUTED_JUMP, SourceType.USER_DEFINED, 0);
                }
                Function f = getFunctionContaining(br);
                if (f == null) { printerr("jumptable " + br + ": not in a function"); nErr++; continue; }
                CreateFunctionCmd.fixupFunctionBody(currentProgram, f, monitor);
                new ghidra.program.model.pcode.JumpTable(br, dests, true, 0).writeOverride(f);
            } catch (Exception ex) {
                printerr("jumptable " + br + ": " + ex.getMessage()); nErr++;
            }
        }
    }

    private void applyBodyAdd(JsonObject d) {
        if (!d.has("body_add")) return;
        for (JsonElement e : d.getAsJsonArray("body_add")) {
            JsonObject o = e.getAsJsonObject();
            Function f = getFunctionAt(addr(o.get("func")));
            if (f == null) { printerr("body_add: no function at " + o.get("func")); nErr++; continue; }
            try {
                ghidra.program.model.address.AddressSet body = new ghidra.program.model.address.AddressSet(f.getBody());
                for (JsonElement r : o.getAsJsonArray("ranges")) {
                    Address a = addr(r.getAsJsonArray().get(0)), b = addr(r.getAsJsonArray().get(1));
                    if (getInstructionAt(a) == null) disassemble(a);
                    Function other = getFunctionContaining(a);
                    if (other != null && other != f) { println("body_add: " + a + " already in " + other.getName()); continue; }
                    body.addRange(a, b);
                }
                f.setBody(body);
            } catch (Exception ex) {
                printerr("body_add " + f.getName() + ": " + ex.getMessage()); nErr++;
            }
        }
    }

    private void applyFlowOverrides(JsonObject d) {
        if (!d.has("flow_overrides")) return;
        for (JsonElement e : d.getAsJsonArray("flow_overrides")) {
            JsonObject o = e.getAsJsonObject();
            Address a = addr(o.get("addr"));
            try {
                if (getInstructionAt(a) == null) disassemble(a);
                Instruction ins = getInstructionAt(a);
                if (ins == null) { printerr("flow override: no instruction at " + a); nErr++; continue; }
                ghidra.program.model.listing.FlowOverride fo =
                    ghidra.program.model.listing.FlowOverride.valueOf(str(o, "type"));
                if (ins.getFlowOverride() != fo) ins.setFlowOverride(fo);
            } catch (Exception ex) {
                printerr("flow override " + a + ": " + ex.getMessage()); nErr++;
            }
        }
    }

    private void applyDisasm(JsonObject d) {
        if (!d.has("disasm")) return;
        for (JsonElement e : d.getAsJsonArray("disasm")) {
            Address a = addr(e);
            if (getInstructionAt(a) == null) disassemble(a);
        }
    }

    // Mark PC-relative literal pool words that are never written as constant data, so the
    // decompiler folds "ldr rX,[pc,#n]" into the actual address/value.
    private void markConstPools() throws Exception {
        Listing listing = currentProgram.getListing();
        ReferenceManager rm = currentProgram.getReferenceManager();
        ghidra.program.model.mem.MemoryBlock img = currentProgram.getMemory().getBlock("drv_image");
        int n = 0;
        for (Instruction ins : listing.getInstructions(img.getStart(), true)) {
            if (!img.contains(ins.getAddress())) break;
            String m = ins.getMnemonicString();
            if (!m.startsWith("ldr") || m.startsWith("ldrb") || m.startsWith("ldrh")) continue;
            if (!ins.getDefaultOperandRepresentation(1).startsWith("[0x")) continue;
            for (Reference r : ins.getReferencesFrom()) {
                if (!r.getReferenceType().isRead()) continue;
                Address t = r.getToAddress();
                if (!img.contains(t) || getInstructionAt(t) != null) continue;
                boolean written = false;
                for (Reference x : rm.getReferencesTo(t)) if (x.getReferenceType().isWrite()) written = true;
                if (written) continue;
                Data dd = getDataAt(t);
                if (dd == null || !dd.isDefined() || dd.getLength() != 4) {
                    clearListing(t, t.add(3));
                    dd = createData(t, new PointerDataType(VoidDataType.dataType, 4));
                }
                ghidra.program.model.data.MutabilitySettingsDefinition.DEF.setChoice(dd,
                    ghidra.program.model.data.MutabilitySettingsDefinition.CONSTANT);
                n++;
            }
        }
        println("const pool words: " + n);
    }

    private void applyCode(JsonObject d) {
        if (!d.has("code")) return;
        for (JsonElement e : d.getAsJsonArray("code")) {
            Address a = addr(e);
            if (getInstructionAt(a) == null) disassemble(a);
            if (getFunctionAt(a) == null) new CreateFunctionCmd(a).applyTo(currentProgram, monitor);
        }
    }

    private void applyFunctions(JsonObject d) {
        if (!d.has("functions")) return;
        FunctionManager fm = currentProgram.getFunctionManager();
        for (JsonElement e : d.getAsJsonArray("functions")) {
            JsonObject o = e.getAsJsonObject();
            Address a = addr(o.get("addr"));
            try {
                if (getInstructionAt(a) == null) disassemble(a);
                Function f = fm.getFunctionAt(a);
                if (f == null) {
                    new CreateFunctionCmd(a).applyTo(currentProgram, monitor);
                    f = fm.getFunctionAt(a);
                }
                if (f == null) { printerr("no function at " + a); nErr++; continue; }
                String name = str(o, "name");
                if (name != null && !name.equals(f.getName())) {
                    for (Symbol sy : currentProgram.getSymbolTable().getSymbols(a))
                        if (sy.getName().equals(name) && sy.getSymbolType() == SymbolType.LABEL) sy.delete();
                    f.setName(name, SourceType.USER_DEFINED);
                }
                String sig = str(o, "signature");
                if (sig != null) {
                    FunctionSignatureParser sp = new FunctionSignatureParser(currentProgram.getDataTypeManager(), null);
                    FunctionDefinitionDataType fd = sp.parse(f.getSignature(), sig);
                    ApplyFunctionSignatureCmd cmd = new ApplyFunctionSignatureCmd(a, fd, SourceType.USER_DEFINED);
                    if (!cmd.applyTo(currentProgram, monitor)) {
                        printerr("sig " + name + ": " + cmd.getStatusMsg()); nErr++;
                    }
                }
                if (o.has("params")) applyStorage(f, o);
                if (o.has("noreturn")) f.setNoReturn(o.get("noreturn").getAsBoolean());
                if (o.has("locals")) pendingLocals.add(o);
                StringBuilder plate = new StringBuilder();
                if (str(o, "plate") != null) plate.append(str(o, "plate"));
                if (str(o, "confidence") != null)
                    plate.append(plate.length() > 0 ? "\n" : "").append("[decomp confidence: ")
                         .append(str(o, "confidence")).append("]");
                if (str(o, "source") != null)
                    plate.append("\n[C: ").append(str(o, "source")).append("]");
                if (plate.length() > 0) f.setComment(plate.toString());
                nFunc++;
            } catch (Exception ex) {
                printerr("function " + a + ": " + ex.getMessage()); nErr++;
            }
        }
    }

    private final List<JsonObject> pendingLocals = new ArrayList<>();

    private void applyStorage(Function f, JsonObject o) throws Exception {
        List<Variable> ps = new ArrayList<>();
        for (JsonElement pe : o.getAsJsonArray("params")) {
            JsonObject p = pe.getAsJsonObject();
            DataType dt = parseType(str(p, "type"));
            Register r = currentProgram.getRegister(str(p, "reg"));
            if (r == null) throw new Exception("bad register " + str(p, "reg"));
            ps.add(new ParameterImpl(str(p, "name"), dt, new VariableStorage(currentProgram, r),
                                     currentProgram, SourceType.USER_DEFINED));
        }
        ReturnParameterImpl ret;
        if (o.has("ret")) {
            JsonObject rj = o.getAsJsonObject("ret");
            ret = new ReturnParameterImpl(parseType(str(rj, "type")),
                new VariableStorage(currentProgram, currentProgram.getRegister(str(rj, "reg"))), currentProgram);
        } else {
            ret = new ReturnParameterImpl(VoidDataType.dataType, VariableStorage.VOID_STORAGE, currentProgram);
        }
        f.updateFunction(null, ret, ps, Function.FunctionUpdateType.CUSTOM_STORAGE, true, SourceType.USER_DEFINED);
    }

    // Decompiler-local renames need the (updated) function signatures, so they run last.
    private void applyLocals() {
        if (pendingLocals.isEmpty()) return;
        DecompInterface di = new DecompInterface();
        DecompileOptions opts = new DecompileOptions();
        opts.grabFromProgram(currentProgram);
        di.setOptions(opts);
        di.openProgram(currentProgram);
        int n = 0;
        for (JsonObject o : pendingLocals) {
            Function f = getFunctionAt(addr(o.get("addr")));
            if (f == null) continue;
            try {
                DecompileResults r = di.decompileFunction(f, 60, monitor);
                HighFunction hf = r.getHighFunction();
                if (hf == null) { printerr("locals: decompile failed " + f.getName()); nErr++; continue; }
                Map<String, HighSymbol> syms = new HashMap<>();
                Iterator<HighSymbol> it = hf.getLocalSymbolMap().getSymbols();
                while (it.hasNext()) {
                    HighSymbol hs = it.next();
                    if (hs.isParameter()) continue;   // never touch pinned parameters
                    syms.put(hs.getName(), hs);
                    syms.put(storageKey(hs), hs);
                }
                Set<String> paramRegs = new HashSet<>();
                for (Parameter p : f.getParameters())
                    for (Varnode vn : p.getVariableStorage().getVarnodes())
                        if (vn.isRegister()) paramRegs.add(currentProgram.getRegister(vn).getName());
                for (Map.Entry<String, JsonElement> e : o.getAsJsonObject("locals").entrySet()) {
                    String newName; DataType dt = null;
                    if (e.getValue().isJsonObject()) {
                        JsonObject v = e.getValue().getAsJsonObject();
                        newName = str(v, "name");
                        if (str(v, "type") != null) dt = parseType(str(v, "type"));
                    } else newName = e.getValue().getAsString();
                    HighSymbol hs = null;
                    for (String k : e.getKey().split("\\|")) if (hs == null) hs = syms.get(k.trim());
                    if (hs == null) {
                        if (!syms.containsKey(newName))
                            println("locals: " + f.getName() + ": no variable " + e.getKey() + " (have " + syms.keySet() + ")");
                        continue;
                    }
                    if (hs.getName().equals(newName) && dt == null) continue;
                    if (syms.containsKey(newName) && syms.get(newName) != hs) {
                        // target name already taken (already applied to another variable, or the
                        // decompiler renumbered); never merge variables
                        continue;
                    }
                    // A register local whose storage is also a parameter register at entry would make
                    // Ghidra merge/drop the parameter when committed; refuse instead of corrupting it.
                    String sk = storageKey(hs);
                    if (sk.startsWith("@") && paramRegs.contains(sk.substring(1, sk.indexOf(':')))
                            && hs.getPCAddress() != null && hs.getPCAddress().equals(f.getEntryPoint())) {
                        println("locals: " + f.getName() + ": " + e.getKey() + " aliases a parameter register at entry; skipped");
                        continue;
                    }
                    try {
                        HighFunctionDBUtil.updateDBVariable(hs, newName, dt, SourceType.USER_DEFINED);
                    } catch (ghidra.util.exception.DuplicateNameException dup) {
                        // a stale DB local (no longer mapped by the decompiler) holds the name
                        boolean removed = false;
                        for (Variable lv : f.getLocalVariables())
                            if (lv.getName().equals(newName)) { f.removeVariable(lv); removed = true; }
                        if (!removed) throw dup;
                        HighFunctionDBUtil.updateDBVariable(hs, newName, dt, SourceType.USER_DEFINED);
                    }
                    n++;
                }
            } catch (Exception ex) {
                printerr("locals " + f.getName() + ": " + ex.getMessage()); nErr++;
            }
        }
        di.dispose();
        println("locals renamed: " + n);
    }

    // Stable key for a decompiler local: "@<reg>:<first-use address>" for register locals,
    // "@stack:<offset>" for stack locals. Unaffected by the decompiler's renumbering.
    static String storageKey(HighSymbol hs) {
        try {
            VariableStorage st = hs.getStorage();
            if (st == null || st.getVarnodes().length == 0) return "@?";
            Varnode vn = st.getFirstVarnode();
            if (st.isRegisterStorage() && st.getRegister() != null)
                return "@" + st.getRegister().getName() + ":" +
                    (hs.getPCAddress() == null ? "?" : "0x" + Long.toHexString(hs.getPCAddress().getOffset()));
            if (st.isStackStorage()) return "@stack:" + st.getStackOffset();
            return "@" + vn.getAddress().getAddressSpace().getName() + ":" + Long.toHexString(vn.getOffset()) +
                (hs.getPCAddress() == null ? "" : ":0x" + Long.toHexString(hs.getPCAddress().getOffset()));
        } catch (Exception e) { return "@?"; }
    }

    // Keep machine-specific absolute import paths out of the program metadata.
    private void applyProgramInfo(JsonObject d) {
        if (!d.has("program_info")) return;
        JsonObject o = d.getAsJsonObject("program_info");
        if (str(o, "executable_path") != null) currentProgram.setExecutablePath(str(o, "executable_path"));
        if (str(o, "fsrl") != null) {
            ghidra.framework.options.Options info = currentProgram.getOptions(Program.PROGRAM_INFO);
            for (String name : info.getOptionNames())
                if (name.toLowerCase().contains("fsrl")) info.setString(name, str(o, "fsrl"));
        }
    }

    private void applyLabels(JsonObject d) {
        if (!d.has("labels")) return;
        SymbolTable st = currentProgram.getSymbolTable();
        for (JsonElement e : d.getAsJsonArray("labels")) {
            JsonObject o = e.getAsJsonObject();
            Address a = addr(o.get("addr"));
            try {
                String name = str(o, "name");
                // Only ever rename *global* labels: symbols in other namespaces (e.g. the
                // override::jmp_* case labels of a jump-table override) must be left alone,
                // or the override silently stops working.
                Symbol s = st.getPrimarySymbol(a);
                if (s != null && !s.getParentNamespace().isGlobal()) {
                    Symbol g = st.getGlobalSymbol(name, a);
                    if (g == null) st.createLabel(a, name, SourceType.USER_DEFINED).setPrimary();
                    else g.setPrimary();
                    s = null;
                    name = null;
                }
                if (name == null) {
                    // handled above
                } else if (s != null && s.getSource() != SourceType.DEFAULT && s.getSymbolType() == SymbolType.LABEL
                        && !s.getName().equals(name)) {
                    s.setName(name, SourceType.USER_DEFINED);
                } else if (s == null || !s.getName().equals(name)) {
                    st.createLabel(a, name, SourceType.USER_DEFINED).setPrimary();
                }
                String type = str(o, "type");
                if (type != null) {
                    DataType dt = parseType(type);
                    if (dt != null) {
                        clearListing(a, a.add(dt.getLength() - 1));
                        createData(a, dt);
                    }
                }
                if (str(o, "comment") != null) setEOLComment(a, str(o, "comment"));
                nLabel++;
            } catch (Exception ex) {
                printerr("label " + a + ": " + ex.getMessage()); nErr++;
            }
        }
    }

    private DataType parseType(String t) throws Exception {
        ghidra.util.data.DataTypeParser p = new ghidra.util.data.DataTypeParser(
            currentProgram.getDataTypeManager(), currentProgram.getDataTypeManager(), null,
            ghidra.util.data.DataTypeParser.AllowedDataTypes.ALL);
        return p.parse(t);
    }

    private void applyComments(JsonObject d) {
        if (!d.has("comments")) return;
        for (JsonElement e : d.getAsJsonArray("comments")) {
            JsonObject o = e.getAsJsonObject();
            Address a = addr(o.get("addr"));
            String k = str(o, "kind");
            String t = str(o, "text");
            if (k == null || k.equals("eol")) setEOLComment(a, t);
            else if (k.equals("pre")) setPreComment(a, t);
            else if (k.equals("post")) setPostComment(a, t);
            else if (k.equals("plate")) setPlateComment(a, t);
            nCmt++;
        }
    }
}
