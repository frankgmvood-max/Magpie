// Own metadata transformation. No commercial IL or BAML is distributed.
// C# 5 deliberately: Windows PowerShell 5.1 compiles this against Cecil net40.
using System;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using Mono.Cecil;
using Mono.Cecil.Cil;

namespace NativeDLSS.Install {
public static class Patcher {
    public const string OriginalHash = "e4ea2dbb1371ea1920d73c87202f19b6f095ef6f521a8a7a139d19f34f1fe866";
    public static readonly string[] IntProperties = {
        "FlowPreset", "FlowGrid", "AnalysisPercent", "Slots", "DuplicateFilter",
        "SceneCut", "DuplicateDelta", "CutDelta", "CutPercent"
    };
    static string Hash(string path) {
        using (var sha = SHA256.Create()) using (var s = File.OpenRead(path))
            return BitConverter.ToString(sha.ComputeHash(s)).Replace("-", "").ToLowerInvariant();
    }
    public static void Patch(string input, string output, string helper) {
        if (Hash(input) != OriginalHash) throw new InvalidDataException("Unsupported managed LS version; nothing patched.");
        Rewrite(input, output, helper, false);
    }
    // Only our source-owned test assembly is admitted, never an arbitrary target.
    public static void PatchFixture(string input, string output, string helper) {
        using (var a = AssemblyDefinition.ReadAssembly(input))
            if (a.Name.Name != "NativeUIFixture") throw new InvalidDataException("Not our test fixture.");
        Rewrite(input, output, helper, true);
    }
    static MethodDefinition Method(TypeDefinition t, string name) {
        return t.Methods.Single(m => m.Name == name);
    }
    static void Prefix(MethodDefinition m, params Instruction[] code) {
        var il = m.Body.GetILProcessor(); var first = m.Body.Instructions[0];
        foreach (var i in code) il.InsertBefore(first, i);
    }
    static void BeforeReturns(MethodDefinition m, Func<Instruction[]> code) {
        // Branches targeting a return must execute the epilogue as well.
        var il = m.Body.GetILProcessor();
        foreach (var ret in m.Body.Instructions.Where(i => i.OpCode == OpCodes.Ret).ToArray()) {
            ret.OpCode = OpCodes.Nop;
            var last = ret;
            foreach (var i in code().Concat(new[] { Instruction.Create(OpCodes.Ret) })) { il.InsertAfter(last, i); last = i; }
        }
    }
    static FieldDefinition Property(TypeDefinition profile, string suffix, TypeReference type) {
        string name = "NativeDLSS" + suffix;
        if (profile.Properties.Any(p => p.Name == name)) throw new InvalidDataException("Already patched profile.");
        var field = new FieldDefinition("_" + name, FieldAttributes.Private, type); profile.Fields.Add(field);
        var get = new MethodDefinition("get_" + name, MethodAttributes.Public | MethodAttributes.SpecialName | MethodAttributes.HideBySig, type);
        get.Body.Instructions.Add(Instruction.Create(OpCodes.Ldarg_0)); get.Body.Instructions.Add(Instruction.Create(OpCodes.Ldfld, field)); get.Body.Instructions.Add(Instruction.Create(OpCodes.Ret));
        var set = new MethodDefinition("set_" + name, MethodAttributes.Public | MethodAttributes.SpecialName | MethodAttributes.HideBySig, profile.Module.TypeSystem.Void);
        set.Parameters.Add(new ParameterDefinition(type));
        set.Body.Instructions.Add(Instruction.Create(OpCodes.Ldarg_0)); set.Body.Instructions.Add(Instruction.Create(OpCodes.Ldarg_1)); set.Body.Instructions.Add(Instruction.Create(OpCodes.Stfld, field)); set.Body.Instructions.Add(Instruction.Create(OpCodes.Ret));
        profile.Methods.Add(get); profile.Methods.Add(set);
        profile.Properties.Add(new PropertyDefinition(name, PropertyAttributes.None, type) { GetMethod = get, SetMethod = set });
        return field;
    }
    static void Rewrite(string input, string output, string helper, bool fixture) {
        using (var assembly = AssemblyDefinition.ReadAssembly(input))
        using (var own = AssemblyDefinition.ReadAssembly(helper)) {
            var m = assembly.MainModule;
            if (!fixture && (m.Mvid != new Guid("55e3bf6f-9537-4ffb-85fd-c18b581bb253") || assembly.Name.Name != "LosslessScaling" || assembly.Name.HasPublicKey))
                throw new InvalidDataException("Managed identity mismatch.");
            var controls = own.MainModule.Types.Single(t => t.FullName == "NativeDLSS.UI.Controls");
            Func<string, MethodReference> hook = name => m.ImportReference(Method(controls, name));
            var profile = m.Types.Single(t => t.FullName == "UI.Profile");
            var page = m.Types.Single(t => t.FullName == "UI.Pages.ProfilePage");
            var main = m.Types.Single(t => t.FullName == "UI.MainWindow");
            var fg = profile.NestedTypes.Single(t => t.Name == "FrameGenerationEnum");
            if (fg.Fields.Any(f => f.Name == "DLSS" || (f.HasConstant && Convert.ToInt32(f.Constant) == 6))) throw new InvalidDataException("FG enum already extended.");
            fg.Fields.Add(new FieldDefinition("DLSS", FieldAttributes.Public | FieldAttributes.Static | FieldAttributes.Literal | FieldAttributes.HasDefault, fg) { Constant = 6 });
            var fields = IntProperties.Select(n => Property(profile, n, m.TypeSystem.Int32)).ToList();
            for (int i = 1; i <= 8; ++i) fields.Add(Property(profile, "HUD" + i, m.TypeSystem.String));
            foreach (var ctor in profile.Methods.Where(c => c.IsConstructor && !c.IsStatic)) {
                var copy = ctor.Parameters.SingleOrDefault(p => p.ParameterType.FullName == profile.FullName);
                if (copy == null) continue;
                BeforeReturns(ctor, () => {
                    var end = Instruction.Create(OpCodes.Nop);
                    return new[] { Instruction.Create(OpCodes.Ldarg, copy), Instruction.Create(OpCodes.Brfalse, end) }
                        .Concat(fields.SelectMany(f => new[] { Instruction.Create(OpCodes.Ldarg_0), Instruction.Create(OpCodes.Ldarg, copy), Instruction.Create(OpCodes.Ldfld, f), Instruction.Create(OpCodes.Stfld, f) }))
                        .Concat(new[] { end }).ToArray();
                });
                Widen(ctor);
            }
            var pageCtor = page.Methods.Single(c => c.IsConstructor && c.Parameters.Count == 1 && c.Parameters[0].ParameterType.FullName == profile.FullName);
            var init = pageCtor.Body.Instructions.Single(i => i.Operand is MethodReference && ((MethodReference)i.Operand).Name == "InitializeComponent");
            var il = pageCtor.Body.GetILProcessor();
            var arg = Instruction.Create(OpCodes.Ldarg_0); il.InsertAfter(init, arg); il.InsertAfter(arg, Instruction.Create(OpCodes.Call, hook("Attach")));
            var apply = Method(page, "ApplyProfileToUI");
            Prefix(apply, Instruction.Create(OpCodes.Ldarg_0), Instruction.Create(OpCodes.Ldarg_1), Instruction.Create(OpCodes.Call, hook("BeginProfile")));
            var getter = apply.Body.Instructions.Single(i => i.Operand is MethodReference && ((MethodReference)i.Operand).Name == "get_FrameGeneration" && i.Next != null && i.Next.Operand is MethodReference && ((MethodReference)i.Next.Operand).Name == "set_SelectedIndex");
            il = apply.Body.GetILProcessor(); arg = Instruction.Create(OpCodes.Ldarg_0); il.InsertAfter(getter, arg); il.InsertAfter(arg, Instruction.Create(OpCodes.Call, hook("TypeIndex")));
            BeforeReturns(apply, () => new[] { Instruction.Create(OpCodes.Ldarg_0), Instruction.Create(OpCodes.Call, hook("EndProfile")) });
            var changed = Method(page, "FrameGeneration_SelectionChanged");
            var original = changed.Body.Instructions[0];
            Prefix(changed, Instruction.Create(OpCodes.Ldarg_0), Instruction.Create(OpCodes.Call, hook("HandleType")), Instruction.Create(OpCodes.Brfalse, original), Instruction.Create(OpCodes.Ret));
            Prefix(Method(main, "ApplyProfileToCore"), Instruction.Create(OpCodes.Ldarg_1), Instruction.Create(OpCodes.Call, hook("ApplyProfile")));
            // Dependencies absent from the commercial deps.json need an explicit
            // load before a patched page is JIT-compiled. Avoid mscorlib imports.
            var runtime = m.AssemblyReferences.Where(r => r.Name == "System.Runtime").OrderByDescending(r => r.Version).First();
            Func<string,string,TypeReference> type = (ns, name) => new TypeReference(ns, name, m, runtime);
            var context = type("System", "AppContext"); var path = type("System.IO", "Path"); var reflection = type("System.Reflection", "Assembly");
            var baseDir = new MethodReference("get_BaseDirectory", m.TypeSystem.String, context);
            var combine = new MethodReference("Combine", m.TypeSystem.String, path);
            combine.Parameters.Add(new ParameterDefinition(m.TypeSystem.String)); combine.Parameters.Add(new ParameterDefinition(m.TypeSystem.String));
            var load = new MethodReference("LoadFrom", reflection, reflection); load.Parameters.Add(new ParameterDefinition(m.TypeSystem.String));
            Prefix(m.EntryPoint, Instruction.Create(OpCodes.Call, baseDir), Instruction.Create(OpCodes.Ldstr, "NativeDLSS.UI.dll"), Instruction.Create(OpCodes.Call, combine), Instruction.Create(OpCodes.Call, load), Instruction.Create(OpCodes.Pop));
            foreach (var changedMethod in new[] { pageCtor, apply, changed, Method(main, "ApplyProfileToCore"), m.EntryPoint }) Widen(changedMethod);
            m.Write(output);
        }
    }
    static void Widen(MethodDefinition method) {
        foreach (var i in method.Body.Instructions) {
            // All short branches, including original ones, can move beyond sbyte.
            if (i.OpCode.OperandType == OperandType.ShortInlineBrTarget)
                i.OpCode = (OpCode)typeof(OpCodes).GetFields().Single(f => f.FieldType == typeof(OpCode) && ((OpCode)f.GetValue(null)).Name == i.OpCode.Name.Substring(0, i.OpCode.Name.Length - 2)).GetValue(null);
        }
    }
}
}
