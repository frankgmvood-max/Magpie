// Ghidra 12.1.4 headless post-script. Outputs generated pseudocode, not source.
// @category LG.Research
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import com.google.gson.GsonBuilder;
import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.regex.Pattern;

public class ExportDolbyFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) throw new IllegalArgumentException("output directory required");
        Path output = Path.of(args[0], currentProgram.getName());
        Files.createDirectories(output);
        Pattern filter = Pattern.compile(args.length > 1 ? args[1] : "dolby|dovi|vsvdb", Pattern.CASE_INSENSITIVE);
        DecompInterface decompiler = new DecompInterface();
        if (!decompiler.openProgram(currentProgram)) throw new IllegalStateException("Cannot open program");
        JsonArray results = new JsonArray();
        try {
            FunctionIterator functions = currentProgram.getFunctionManager().getFunctions(true);
            while (functions.hasNext() && !monitor.isCancelled()) {
                Function function = functions.next();
                String name = function.getName(true);
                if (!filter.matcher(name).find() || function.isExternal() || function.isThunk()) continue;
                JsonObject record = new JsonObject();
                record.addProperty("name", name);
                record.addProperty("entry", function.getEntryPoint().toString());
                record.addProperty("prototype", function.getPrototypeString(false, false));
                record.addProperty("image_base", currentProgram.getImageBase().toString());
                JsonArray called = new JsonArray();
                for (Function callee : function.getCalledFunctions(monitor)) called.add(callee.getName(true));
                record.add("calls", called);
                JsonArray callers = new JsonArray();
                for (Function caller : function.getCallingFunctions(monitor)) callers.add(caller.getName(true));
                record.add("callers", callers);
                DecompileResults result = decompiler.decompileFunction(function, 60, monitor);
                record.addProperty("decompiled", result.decompileCompleted());
                if (result.decompileCompleted()) {
                    String safeName = name.replaceAll("[^a-zA-Z0-9_.-]", "_");
                    if (safeName.length() > 150) safeName = safeName.substring(0, 140) + "_" + Integer.toHexString(name.hashCode());
                    String file = function.getEntryPoint() + "_" + safeName + ".c";
                    Files.writeString(output.resolve(file), "/* Ghidra-generated pseudocode; inferred types may be wrong. */\n" + result.getDecompiledFunction().getC(), StandardCharsets.UTF_8);
                    record.addProperty("pseudocode_file", file);
                } else record.addProperty("error", result.getErrorMessage());
                results.add(record);
            }
        } finally {
            decompiler.dispose();
        }
        String indexName = "functions-" + Integer.toHexString(filter.pattern().hashCode()) + ".json";
        Files.writeString(output.resolve(indexName), new GsonBuilder().setPrettyPrinting().create().toJson(results) + "\n", StandardCharsets.UTF_8);
        println("Exported " + results.size() + " functions to " + output);
    }
}
