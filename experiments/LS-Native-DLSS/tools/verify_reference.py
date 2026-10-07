#!/usr/bin/env python3
"""Reproduce the checked inventory against a locally owned LS DLL."""
import argparse
import json
from pathlib import Path
from analyze_dll import analyze

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument("dll", type=Path)
ap.add_argument("--reference", type=Path, default=Path(__file__).parents[1] / "analysis/ls-3.2.2-native.json")
args = ap.parse_args()
actual = analyze(args.dll.read_bytes())
reference = json.loads(args.reference.read_text(encoding="utf-8"))
if actual != reference:
    raise SystemExit("FAIL: reference differs; do not enable a replacement on this input")
assert len(actual["shaders"]) == 202 and not actual["errors"]
synthesis = next(s for s in actual["shaders"] if s["resource_path"] == [10, 256, 1033])
assert synthesis["program"]["thread_group"] == [16, 16, 1]
cb = synthesis["reflection"]["constant_buffers"][0]
assert cb["size"] == 32
timestamp = next(v for v in cb["variables"] if v["name"] == "Timestamp")
assert timestamp["offset"] == 28 and timestamp["size"] == 4 and timestamp["type_id"] == 3
assert timestamp["flags"] & 2
print("PASS: exact DLL/shader metadata match, 202 shaders, synthesis candidate layout verified")
