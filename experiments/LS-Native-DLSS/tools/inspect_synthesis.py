#!/usr/bin/env python3
"""Local read-only inspection through external Wine vkd3d and SPIR-V Tools.

The original DLL is parsed as bytes, never loaded/executed. Proprietary bytecode
and disassembly stay in the ignored local output directory.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from analyze_dll import PE
from analyze_trace import DLL_HASH, SHADER_HASH


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("native", type=Path)
    parser.add_argument("--output", type=Path, default=Path("local/synthesis"))
    parser.add_argument("--vkd3d-compiler", default="vkd3d-compiler")
    parser.add_argument("--spirv-dis", default="spirv-dis")
    parser.add_argument("--spirv-val", default="spirv-val")
    args = parser.parse_args()
    try:
        data = args.native.read_bytes()
        if hashlib.sha256(data).hexdigest() != DLL_HASH:
            raise ValueError("unsupported native DLL SHA256")
        pe = PE(data)
        resource = next(r for r in pe.resources() if r["path"] == [10, 256, 1033])
        shader = pe.bytes(resource["offset"], resource["size"])
        if hashlib.sha256(shader).hexdigest() != SHADER_HASH:
            raise ValueError("unexpected shader payload")
        # Explicit source/target types avoid tool-version autodetection changes.
        result = subprocess.run([args.vkd3d_compiler, "-x", "dxbc-tpf", "-b", "spirv-binary"],
                                input=shader, check=True, capture_output=True)
        args.output.mkdir(parents=True, exist_ok=True)
        spv = args.output / "resource-256.spv"
        spv.write_bytes(result.stdout)
        subprocess.run([args.spirv_val, str(spv)], check=True)
        assembly = args.output / "resource-256.spvasm"
        subprocess.run([args.spirv_dis, str(spv), "-o", str(assembly)], check=True)
        version = subprocess.run([args.vkd3d_compiler, "--version"], check=True,
                                 capture_output=True, text=True).stdout.strip()
        manifest = dict(schema_version=1, dll_sha256=DLL_HASH, resource_path=[10,256,1033],
                        shader_sha256=SHADER_HASH, shader_size=len(shader), compiler_version=version,
                        translated_sha256=hashlib.sha256(result.stdout).hexdigest(),
                        spirv_validated=True, native_shader_executed=False,
                        pixel_equivalence_verified=False)
        (args.output / "provenance.json").write_text(json.dumps(manifest, indent=2) + "\n")
        print(f"Local synthesis inspection written to {args.output}; no GPU execution")
    except (OSError, ValueError, StopIteration, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"Inspection failed: {exc}\n")


if __name__ == "__main__":
    main()
