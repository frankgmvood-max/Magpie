# Native UI startup correction, 0.3.1

REA 6.3.0 was invoked again through its actual upstream MCP provider
`rea-dotnet-static`: `inspect_managed_artifact` and `inspect_managed_members`
against the exact original managed LS assembly SHA256
`e4ea2dbb1371ea1920d73c87202f19b6f095ef6f521a8a7a139d19f34f1fe866`.
Member evidence: `ev_fcf726fe9f208bc83f70bd68383e4964f1b74914069604ea4958c79554f54f4e`.
REA reports partial member coverage with no issues; it does not run WPF.

REA identifies the ProfilePage constructor (0x060001ae), component initializer
(0x060001db), connector (0x060001dc), and FG selection handler (0x060001bb).
The connector references both the FG handler and `add_SelectionChanged`.
The original handler begins with the `get_IsLoaded` guard.

Our separate bounded CIL/BAML decoding, using dnfile/dncil and the official
Microsoft WPF record definitions, establishes the missing ordering detail:
connection ID 5 wires the FrameGeneration ComboBox and its selection event;
the same element subsequently receives SelectedIndex=0 through a BAML
PropertyWithConverter record. All this occurs inside InitializeComponent.
Our injected Attach call runs after that method returns. This decoding is
our analysis, not a REA decompilation or REA runtime observation. Raw commercial
CIL/BAML and original/patched binaries remain private and are not distributed.

The 0.3.0 HandleType hook ran before the original IsLoaded guard and threw
InvalidOperationException when its ConditionalWeakTable did not yet contain
the page. The correction returns to the original handler while the view is
absent; after attachment, the existing profile loading and DLSS/native selection
logic is retained. Native GPU generation, scheduling and Present are unchanged.

The source-owned WPF fixture now sets SelectedIndex=0 after wiring the event
and before creating other page controls. The build also compiles the exact
shipped helper source from commit 74fcdc811d099baf6635c316d488e690b6f9acd2
as a negative control. The fixture must reproduce the early HandleType exception
with that source and pass on .NET 8 and .NET 9 with the corrected helper.
The baseline uses the current project metadata; the replay concerns shipped
helper logic, not identical historical binary bytes.

Start-Diagnostics captures apphost stderr/stdout, child-process-only .NET 9
host trace and exit code. Collect-Logs includes these outputs and managed/helper
hashes, without adding commercial DLLs or profile contents. A controlled failing
console process tests the diagnostic launcher's working directory and output.

The full commercial application's startup on the user's computer and physical
NVIDIA/SM86/VRR validation remain unverified until the user tests this build.
This corrects a demonstrated startup defect; it is not proof that the user's
computer has no additional launch failure.
