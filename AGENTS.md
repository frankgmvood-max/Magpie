# Agent instructions for this fork

Follow the user's requested branch and task scope. Follow the existing project
style in `CONTRIBUTING.md` and any more specific instructions in subdirectories.

## Tool selection

For substantial code analysis, reverse engineering, browser debugging, profiling,
or repository-wide edits, consult `tools/agent-toolbox/README.md`. It contains the
tool routes, a machine-readable catalog, and the dated research behind the choices.
Skip this extra process for trivial changes that do not benefit from another tool.

1. Prefer suitable capabilities already available in the current environment.
2. Select the smallest useful route with
   `python tools/agent-toolbox/toolbox.py plan TASK`.
3. Check command availability with
   `python tools/agent-toolbox/toolbox.py doctor --task TASK`.
4. If a required tool is missing, read its current official setup documentation
   from the catalog and set it up in the task environment when authorized and
   supported. Install only what the task needs, using reproducible versions.
5. Use the tool's actual results to support findings and validate changed behavior.

The catalog is a reference, not proof that software or an MCP connection is
installed. A command found on PATH also does not prove a server is connected or
that it supports a requested operation. Report unavailable capabilities plainly.
Adding this repository does not grant global capabilities to other chat sessions.

## Choosing the right level of analysis

- Native EXE/DLL: Detect It Easy, Ghidra, then GhidraMCP when AI integration helps.
- Execution behavior: x64dbg or Frida, depending on the platform and task.
- Managed .NET: dnSpyEx; Android DEX/APK: JADX; binary formats: ImHex.
- Source navigation/refactoring: Serena, Context7, and ast-grep as appropriate.
- Browser verification: Playwright CLI; performance/network/memory: Chrome DevTools.
- Python performance: py-spy; native/frame performance: Tracy when instrumentation
  is available. Confirm platform requirements before choosing either.

Prefer the official Hex-Rays IDA MCP when a compatible licensed IDA is available.
Roo Code and Continue are retained as historical catalog entries, not default
choices. Check current support and licensing before adopting a dated recommendation.
GitNexus uses a noncommercial license; do not treat it as an unrestricted default
for commercial tasks. The catalog includes alternative tools for source analysis.

Preserve the original binaries. Use appropriate isolation for untrusted programs.
Separate observed behavior from hypotheses about decompiled code. Never claim
that a guessed function name or model-generated reconstruction is original source.
