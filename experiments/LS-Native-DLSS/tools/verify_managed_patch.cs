// Independent semantic verification over private target files, not execution.
using System;
using System.IO;
using System.Linq;
using System.Collections.Generic;
using System.Security.Cryptography;
using Mono.Cecil;
using Mono.Cecil.Cil;
class VerifyManaged {
    static IEnumerable<TypeDefinition> Types(TypeDefinition t) { yield return t; foreach (var c in t.NestedTypes) foreach (var x in Types(c)) yield return x; }
    static string Operand(MethodDefinition m, object o) {
        if (o == null) return "";
        if (o is Instruction) return "target:" + m.Body.Instructions.IndexOf((Instruction)o);
        if (o is Instruction[]) return string.Join(",", ((Instruction[])o).Select(i => m.Body.Instructions.IndexOf(i)));
        if (o is MemberReference) return ((MemberReference)o).FullName;
        if (o is ParameterDefinition) return "arg:" + ((ParameterDefinition)o).Index;
        if (o is VariableDefinition) return "local:" + ((VariableDefinition)o).Index;
        return o.ToString();
    }
    static string[] Body(MethodDefinition m) { return m.Body.Instructions.Select(i => i.OpCode.Name + ":" + Operand(m,i.Operand)).ToArray(); }
    static string Digest(byte[] bytes) { using (var sha = SHA256.Create()) return BitConverter.ToString(sha.ComputeHash(bytes)); }
    static int Main(string[] args) {
        using (var a = AssemblyDefinition.ReadAssembly(args[0])) using (var b = AssemblyDefinition.ReadAssembly(args[1])) {
            if (a.MainModule.Architecture != b.MainModule.Architecture || a.MainModule.Attributes != b.MainModule.Attributes || a.Name.FullName != b.Name.FullName) throw new Exception("Assembly identity/flags changed");
            var originals = a.MainModule.Types.SelectMany(Types).SelectMany(t => t.Methods).ToArray();
            var patched = b.MainModule.Types.SelectMany(Types).SelectMany(t => t.Methods).ToDictionary(m => m.FullName);
            var changed = new List<string>();
            foreach (var m in originals) {
                var n = patched[m.FullName];
                if (m.Attributes != n.Attributes || m.ImplAttributes != n.ImplAttributes || m.HasBody != n.HasBody) throw new Exception("Original method declaration changed");
                if (!m.HasBody) {
                    if (m.HasPInvokeInfo && (m.PInvokeInfo.Attributes != n.PInvokeInfo.Attributes || m.PInvokeInfo.EntryPoint != n.PInvokeInfo.EntryPoint || m.PInvokeInfo.Module.Name != n.PInvokeInfo.Module.Name)) throw new Exception("Native boundary changed");
                    continue;
                }
                if (!Body(m).SequenceEqual(Body(n))) changed.Add(m.FullName);
            }
            string[] expected = {
                "System.Void UI.App::Main()",
                "System.Void UI.MainWindow::ApplyProfileToCore(UI.Profile)",
                "System.Void UI.Profile::.ctor(System.String,System.String,System.Boolean,System.Int32,UI.Profile)",
                "System.Void UI.Pages.ProfilePage::.ctor(UI.Profile)",
                "System.Void UI.Pages.ProfilePage::ApplyProfileToUI(UI.Profile)",
                "System.Void UI.Pages.ProfilePage::FrameGeneration_SelectionChanged(System.Object,System.Windows.Controls.SelectionChangedEventArgs)"
            };
            if (!changed.OrderBy(x=>x).SequenceEqual(expected.OrderBy(x=>x))) throw new Exception("Unexpected original method modifications: " + string.Join("; ", changed));
            foreach (var r in a.MainModule.Resources.OfType<EmbeddedResource>()) {
                var n = b.MainModule.Resources.OfType<EmbeddedResource>().Single(x => x.Name == r.Name);
                if (Digest(r.GetResourceData()) != Digest(n.GetResourceData())) throw new Exception("Commercial BAML/localization changed");
            }
            Console.WriteLine("PASS: " + originals.Length + " original declarations retained; exactly six intended method hooks; all original P/Invoke declarations and embedded resource bytes preserved.");
            return 0;
        }
    }
}
