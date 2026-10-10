#!/usr/bin/env python3
import argparse
import json
from pathlib import Path
from analyze_dll import analyze

parser = argparse.ArgumentParser()
parser.add_argument("proxy", type=Path)
args = parser.parse_args()
reference = json.loads((Path(__file__).parents[1] / "analysis/ls-3.2.2-native.json").read_text())
actual = analyze(args.proxy.read_bytes())
expected = [(r["name"], r["ordinal"]) for r in reference["exports"]]
expected.append(("ZNativeDLSSConfigure", 12))
got = [(r["name"], r["ordinal"]) for r in actual["exports"]]
if got != expected or sum(r["forwarded"] for r in actual["exports"]) != 10:
    parser.exit(1, f"Wrong native export table: {got}\n")
print("PASS: all 11 native names/ordinals preserved; UI bridge at 12; 10 PE forwarders")
