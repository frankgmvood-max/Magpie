#!/usr/bin/env python3
"""Search extracted regular files for literal feature names, skipping symlinks."""

import argparse
import json
from pathlib import Path


def scan(root, needles):
    result = {"regular_files": 0, "bytes": 0, "matches": [], "errors": []}
    for path in sorted(root.rglob("*")):
        if path.is_symlink() or not path.is_file():
            continue
        relative = path.relative_to(root).as_posix()
        try:
            data = path.read_bytes()
        except OSError as error:
            result["errors"].append({"path": relative, "error": str(error)})
            continue
        result["regular_files"] += 1
        result["bytes"] += len(data)
        lowered = data.lower()
        hits = [n for n in needles if n.lower().encode("ascii") in lowered]
        if hits:
            result["matches"].append({"path": relative, "needles": hits})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", action="append", required=True, help="label=directory")
    parser.add_argument("--needle", action="append")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    needles = args.needle or ["enableDolbyVisionPC", "dolbyVisionPC", "dolby_vision_pc", "dv_pc"]
    report = {"needles": needles, "scope": "Case-insensitive literal bytes; absence does not prove absence of equivalent logic.", "roots": {}}
    for specification in args.root:
        label, directory = specification.split("=", 1)
        report["roots"][label] = scan(Path(directory), needles)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Wrote literal-feature scan to {args.output}")


if __name__ == "__main__":
    main()
