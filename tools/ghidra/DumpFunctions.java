// Dump decompiled C for functions in a headless Ghidra run.
//
// Usage (from analyzeHeadless, after the program is imported and analysed):
//   -postScript DumpFunctions.java <out.c> [all|stub]
//
// "stub" (default) dumps only the functions in the protection sections
// (names starting with "stxt" or "PSFD") and the function containing the
// entry point. "all" dumps every function Ghidra found.
//
// @category VT3

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Program;
import ghidra.program.model.mem.MemoryBlock;

import java.io.FileWriter;
import java.io.PrintWriter;

public class DumpFunctions extends GhidraScript {

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        String outPath = args.length > 0 ? args[0] : "functions.c";
        String mode = args.length > 1 ? args[1] : "stub";

        Program program = currentProgram;
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(program);

        Address entry = null;
        var eps = program.getSymbolTable().getExternalEntryPointIterator();
        if (eps.hasNext()) {
            entry = eps.next();
        }

        PrintWriter out = new PrintWriter(new FileWriter(outPath));
        out.println("// Ghidra decompilation dump for " + program.getName());
        out.println("// entry=" + entry + " mode=" + mode);

        int dumped = 0;
        int failed = 0;
        FunctionIterator functions = program.getFunctionManager().getFunctions(true);
        while (functions.hasNext() && !monitor.isCancelled()) {
            Function f = functions.next();
            if (!"all".equals(mode) && !wantedInStub(program, f, entry)) {
                continue;
            }
            out.println();
            out.println("// ==== " + f.getName() + " @ " + f.getEntryPoint());
            DecompileResults r = decomp.decompileFunction(f, 120, monitor);
            if (r != null && r.decompileCompleted() && r.getDecompiledFunction() != null) {
                out.println(r.getDecompiledFunction().getC());
                dumped++;
            } else {
                out.println("// decompile failed: " + (r == null ? "null" : r.getErrorMessage()));
                failed++;
            }
        }
        out.close();
        println("dumped " + dumped + " functions, " + failed + " failed, to " + outPath);
    }

    private boolean wantedInStub(Program program, Function f, Address entry) {
        if (entry != null && f.getBody().contains(entry)) {
            return true;
        }
        MemoryBlock block = program.getMemory().getBlock(f.getEntryPoint());
        if (block == null) {
            return false;
        }
        String name = block.getName();
        return name.startsWith("stxt") || name.startsWith("PSFD");
    }
}
