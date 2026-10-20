#!/usr/bin/env python3
"""Build compact provenance and comparison records from actual tool outputs."""

import argparse
from collections import defaultdict
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, default=Path("work"))
    parser.add_argument("--output", type=Path, default=Path("evidence"))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    records = {}
    for model in ("c1", "c3"):
        files = json.loads((args.work / f"{model}-hdmi-inventory.json").read_text())
        records[model] = []
        for entry in files:
            record = {k: entry[k] for k in ("filename", "sha256", "bytes", "elf_class", "machine")}
            selected = ("is_dolby_valid", "is_dolby_hdr_supported", "is_dolby_pc_supported", "rule_dolby_hdr",
                        "GetDolbyCriterionType", "GetDolbyVsvdbByType")
            record["functions"] = [
                {k: symbol[k] for k in ("symbol", "demangled", "address", "size")}
                for symbol in entry["symbols"] if any(n in symbol["demangled"] for n in selected)
            ]
            records[model].append(record)
    (args.output / "binaries.json").write_text(json.dumps(records, indent=2) + "\n")
    filenames = {
        "c1": "c1-dolby-gate-emulation", "c3": "c3-dolby-gate-emulation",
        "c1-swap-experiment": "c1-rgb-full-experiment",
        "c1-extend-experiment": "c1-rgb-both-experiment",
    }
    inputs = {key: json.loads((args.work / f"{name}.json").read_text()) for key, name in filenames.items()}
    results = {}
    for key, entry in inputs.items():
        record = {k: entry[k] for k in ("library", "sha256", "predicate_symbol", "predicate_elf_vma",
                                       "predicate_size", "timing_argument_present", "emulator_only_mutation", "scope", "labels")}
        record["case_count"] = len(entry["cases"])
        record["accept_count"] = sum(case["accepted"] for case in entry["cases"])
        groups = defaultdict(set)
        for case in entry["cases"]:
            if case["accepted"]:
                field = "rgb_range" if case["pixel_encoding"] == 0 else "ycc_range"
                group = (case["timing_u16_0x14"], case["timing_u16_0x16"],
                         case["dolby_type"], case["pixel_encoding"], field)
                groups[group].add(case[field])
        record["accepted_groups"] = [
            dict(zip(("timing_u16_0x14", "timing_u16_0x16", "dolby_type", "pixel_encoding", "range_field"), group),
                 accepted_ranges=sorted(values))
            for group, values in sorted(groups.items())
        ]
        results[key] = record
    for key in ("c1-swap-experiment", "c1-extend-experiment"):
        original = inputs["c1"]["cases"]
        experiment = inputs[key]["cases"]
        if len(original) != len(experiment):
            raise ValueError("Case counts do not match")
        changes = []
        for before, after in zip(original, experiment):
            parameters = {k: v for k, v in before.items() if k != "accepted"}
            if parameters != {k: v for k, v in after.items() if k != "accepted"}:
                raise ValueError("Input matrices differ")
            if before["accepted"] != after["accepted"]:
                changes.append({"input": parameters, "before": before["accepted"], "after": after["accepted"]})
        results[key]["changed_cases"] = changes
        results[key]["removed_accepts"] = sum(c["before"] and not c["after"] for c in changes)
    candidate = results["c1-extend-experiment"]
    if len(candidate["changed_cases"]) != 3 or candidate["removed_accepts"]:
        raise ValueError("Candidate did not retain the expected isolated-predicate behavior")
    if not all(c["input"]["dolby_type"] == 3 and c["input"]["pixel_encoding"] == 0
               and c["input"]["rgb_range"] == 2 and c["after"] for c in candidate["changed_cases"]):
        raise ValueError("Unexpected candidate behavior")
    (args.output / "gate-results.json").write_text(json.dumps(results, indent=2) + "\n")
    print("Summarized actual ELF inventories and emulation results; candidate comparison passed")


if __name__ == "__main__":
    main()
