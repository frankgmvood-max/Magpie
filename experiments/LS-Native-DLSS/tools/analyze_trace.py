#!/usr/bin/env python3
"""Validate metadata-only native observations; never claim VRR/frame IDs from them."""
import argparse
import collections
import json
import math
from pathlib import Path
import struct

DLL_HASH = "626b196d799606cd4250b7b29e04228692ab70cf56a5d1bbb56d748c8219f0eb"
INPUT_SHADER_HASH = "214a8ad496c0ffff58e9739d4be085a450d82eaaf23e2ad21dea8b251872880f"
SHADER_HASH = "3af0031e97f43a9372c23749ebbbe91506119268d84e869ba715ffe71bb9d45a"


def uint(value, bits=64):
    return type(value) is int and 0 <= value < (1 << bits)


def float_bits(bits):
    if not uint(bits, 32):
        raise ValueError("not a uint32 float encoding")
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def analyze(rows):
    errors, warnings = [], []
    header = rows[0] if rows and rows[0].get("kind") == "header" else {}
    footer = rows[-1] if rows and rows[-1].get("kind") == "footer" else {}
    if not header:
        errors.append("missing initial header")
    if not footer:
        errors.append("missing final footer: capture interrupted or initialization failed")
    if header.get("schema_version") not in (1, 2) or header.get("dll_sha256") != DLL_HASH or header.get("shader_sha256") != SHADER_HASH:
        errors.append("unsupported schema or native/shader identity")
    if header.get("resource_id") != 256 or header.get("replacement_enabled") is not False or header.get("present_owner") != "LS":
        errors.append("unexpected pass or ownership declaration")
    if header.get("source_frame_ids_known") is not False:
        errors.append("object IDs must not be declared source frame IDs")
    schema = header.get("schema_version")
    if schema == 2 and (header.get("input_resource_id") != 254 or header.get("input_shader_sha256") != INPUT_SHADER_HASH
                        or header.get("source_mutation_coverage_complete") is not False
                        or header.get("source_content_verified") is not False):
        errors.append("unsupported input shader or overstated source-content provenance")
    frequency = header.get("qpc_frequency")
    if not uint(frequency) or not frequency:
        errors.append("invalid QPC frequency")
    records = [r for r in rows[1:] if r.get("kind") in (("dispatch", "input_update") if schema == 2 else ("dispatch",))]
    if len(rows) != len(records) + bool(header) + bool(footer):
        errors.append("unexpected, repeated or misplaced records")
    if not any(r.get("kind") == "dispatch" for r in records):
        errors.append("no identified synthesis dispatches")

    phases = collections.Counter()
    adapters, devices, contexts, dimensions, widths = set(), set(), set(), set(), set()
    sequences = []
    unknown = 0
    object_devices = {}
    for index, row in enumerate(records):
        input_update = row.get("kind") == "input_update"
        prefix = f"{row.get('kind')} {index + 1}: "
        integers = ["sequence", "qpc", "device", "context", "adapter_luid", "thread"]
        if any(not uint(row.get(k)) or not row[k] for k in integers):
            errors.append(prefix + "missing/invalid dispatch or device identity")
            continue
        sequences.append(row["sequence"])
        adapters.add(row["adapter_luid"])
        devices.add(row["device"])
        contexts.add(row["context"])
        key = (row["device"], row["adapter_luid"])
        if input_update:
            pass  # Timestamp/scale are not consumed by the input-copy kernel.
        elif row.get("constants_known") is True:
            try:
                phase = float_bits(row.get("phase_bits"))
                scale = float_bits(row.get("resolution_scale_bits"))
            except ValueError as exc:
                errors.append(prefix + str(exc))
            else:
                if not math.isfinite(phase) or not 0 <= phase <= 1 or not math.isfinite(scale) or scale <= 0:
                    errors.append(prefix + "invalid phase/scale")
                phases[f"0x{row['phase_bits']:08x} ({phase:g})"] += 1
            width, first, count = (row.get(k) for k in ("cb_byte_width", "first_constant", "num_constants"))
            if not all(uint(v, 32) for v in (width, first, count)) or not 32 <= width <= 64 or count < 2 or first * 16 + 32 > width:
                errors.append(prefix + "constant snapshot range does not contain the shader constants")
            else:
                widths.add(width)
        elif row.get("constants_known") is False:
            unknown += 1
        else:
            errors.append(prefix + "invalid constants_known flag")
        inputs, output, groups = row.get("inputs"), row.get("output"), row.get("groups")
        if not isinstance(inputs, list) or len(inputs) != (1 if input_update else 5) or not isinstance(output, dict):
            errors.append(prefix + "invalid binding list")
            continue
        valid_bindings = True
        for binding in [*inputs, output]:
            fields = ("object", "width", "height", "format", "view_format", "mip", "array_size", "samples", "bind_flags")
            if not isinstance(binding, dict) or any(not uint(binding.get(k)) for k in fields):
                errors.append(prefix + "malformed texture metadata")
                valid_bindings = False
                continue
            if not binding["object"] or not binding["width"] or not binding["height"] or binding["samples"] != 1 or binding["array_size"] != 1 or binding["mip"] > 31:
                errors.append(prefix + "missing or unsupported texture view")
                valid_bindings = False
                continue
            prior = object_devices.setdefault(binding["object"], key)
            if prior != key:
                errors.append(prefix + "texture object identity used across devices/adapters")
        if not valid_bindings:
            continue
        if not input_update and inputs[0]["object"] == inputs[1]["object"]:
            warnings.append(prefix + "t0 and t1 reference the same resource; no distinct source pair is established")
        if output["object"] in {r["object"] for r in inputs}:
            errors.append(prefix + "output aliases an input resource")
        width = max(1, output["width"] >> output["mip"])
        height = max(1, output["height"] >> output["mip"])
        if not input_update:
            dimensions.add((width, height, output["view_format"]))
        group_size = 8 if input_update else 16
        if groups != [(width + group_size - 1) // group_size, (height + group_size - 1) // group_size, 1]:
            errors.append(prefix + "Dispatch groups disagree with output mip dimensions")

    if sorted(sequences) != list(range(1, len(records) + 1)):
        errors.append("duplicate/missing dispatch sequences; observation is incomplete")
    if footer:
        counters = ("written", "seen", "dropped_queue", "dropped_busy", "untagged", "tagged_shaders", "tagged_buffers")
        if any(not uint(footer.get(k)) for k in counters):
            errors.append("malformed footer counters")
        else:
            if footer["written"] != len(records) or footer["seen"] < len(records):
                errors.append("footer counters disagree with captured records")
            if footer["dropped_queue"] or footer["dropped_busy"] or footer["untagged"]:
                errors.append("observer dropped records or failed to tag a required object/hook")
            if not footer["tagged_shaders"] or not footer["tagged_buffers"]:
                errors.append("shader/immutable-buffer tagging was not observed")
    if unknown:
        errors.append(f"{unknown} dispatches have unknown constant contents")
    if phases and any(not p.startswith("0x3f000000 ") for p in phases):
        warnings.append("non-midpoint phases observed: the current fixed x2 DLSS slot policy cannot replace these slots")
    pair_matches = validate_source_updates(records, errors, warnings) if schema == 2 else 0
    return {
        "schema_version": schema, "observation_valid": not errors,
        "records": len(records), "input_updates": sum(r.get("kind") == "input_update" for r in records),
        "pairs_matching_observed_updates": pair_matches, "phase_histogram": dict(sorted(phases.items())),
        "constant_buffer_sizes": sorted(widths),
        "adapter_luids": [f"0x{x:016x}" for x in sorted(adapters)],
        "devices": len(devices), "contexts": len(contexts),
        "output_dimensions_and_formats": [list(d) for d in sorted(dimensions)],
        "errors": errors, "warnings": warnings,
        "source_mutation_coverage_complete": False, "source_content_verified": False,
        "replacement_verified": False, "source_frame_ids_verified": False,
        "physical_vrr_verified": False,
        "limits": ["Input-update generations identify observed submissions, not actual game frames or complete mutation coverage.",
                   "A pre-Dispatch record does not establish GPU completion or displayed frame order.",
                   "This metadata cannot prove native frame pacing, HDR semantics, or game motion-vector packing."]
    }


def validate_source_updates(records, errors, warnings):
    """Replay CPU submission stamps. Missing writer coverage never authorizes replacement."""
    histories, epoch_owners = {}, {}
    matches = unmatched = 0
    identity = ("device", "context", "adapter_luid", "thread")
    stamp_ids = ("epoch", "generation", "sequence", "qpc", *identity)
    for row in sorted(records, key=lambda r: r.get("sequence", -1) if uint(r.get("sequence")) else -1):
        prefix = f"record {row.get('sequence')}: "
        key = tuple(row.get(k) for k in identity)
        if any(not uint(v) or not v for v in key):
            continue
        if row.get("kind") == "input_update":
            w = row.get("source_write")
            if not isinstance(w, dict) or any(not uint(w.get(k)) or not w[k] for k in stamp_ids) or not uint(w.get("command_epoch")):
                errors.append(prefix + "invalid source-write stamp")
                continue
            if any(w[k] != row.get(k) for k in ("sequence", "qpc", *identity)) or w.get("texture") != row.get("output"):
                errors.append(prefix + "source-write stamp disagrees with the submitted update")
            texture = w.get("texture", {})
            if (not isinstance(texture, dict) or texture.get("mip") != 0 or texture.get("samples") != 1
                    or texture.get("array_size") != 1 or not texture.get("object")
                    or texture.get("format") != texture.get("view_format")):
                errors.append(prefix + "unsupported source-write view")
                continue
            old = histories.get(key, [])
            if w["epoch"] in epoch_owners and old and old[-1]["epoch"] != w["epoch"]:
                errors.append(prefix + "retired source epoch was reused")
            owner = epoch_owners.setdefault(w["epoch"], key)
            if owner != key:
                errors.append(prefix + "source epoch reused across devices, contexts or threads")
            if old and old[-1]["epoch"] == w["epoch"]:
                last = old[-1]
                compatible = (last["generation"] + 1 == w["generation"] and last["qpc"] <= w["qpc"]
                              and last["command_epoch"] == w["command_epoch"]
                              and last["texture"].get("object") != texture.get("object")
                              and all(last["texture"].get(k) == texture.get(k) for k in ("width", "height", "format", "view_format")))
                if not compatible:
                    errors.append(prefix + "source generation, extent, order or command epoch is inconsistent")
                histories[key] = [last, w]
            else:
                if w["generation"] != 1:
                    errors.append(prefix + "source session starts without generation one")
                histories[key] = [w]
        else:
            declared = row.get("pair_matches_observed_updates")
            if type(declared) is not bool:
                errors.append(prefix + "missing source-pair declaration")
                continue
            if not declared:
                unmatched += 1
                continue
            history = histories.get(key, [])
            inputs = row.get("inputs")
            good = (len(history) == 2 and isinstance(inputs, list) and len(inputs) >= 2)
            if good:
                a, b = history
                good = (row.get("previous_write") == a and row.get("current_write") == b
                        and a["generation"] + 1 == b["generation"] and a["epoch"] == b["epoch"]
                        and a["command_epoch"] == b["command_epoch"]
                        and a["texture"] == inputs[0] and b["texture"] == inputs[1]
                        and uint(row.get("qpc")) and row["qpc"] >= b["qpc"]
                        and uint(row.get("sequence")) and row["sequence"] > b["sequence"])
            if not good:
                errors.append(prefix + "declared pair does not match the latest two observed input writes")
            else:
                matches += 1
    if unmatched:
        warnings.append(f"{unmatched} synthesis dispatches have no matching observed input pair; native fallback required")
    if not any(r.get("kind") == "input_update" for r in records):
        warnings.append("no identified input updates; texture content generations remain unknown")
    return matches


def read_trace(path):
    rows = []
    with path.open(encoding="utf-8") as file:
        for line_number, line in enumerate(file, 1):
            if len(line) > 16384 or len(rows) > 65537:
                raise ValueError("capture exceeds observer bounds")
            try:
                row = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"line {line_number}: interrupted/invalid JSON") from exc
            if not isinstance(row, dict):
                raise ValueError(f"line {line_number}: record must be an object")
            rows.append(row)
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        result = analyze(read_trace(args.trace))
    except (OSError, ValueError, KeyError, TypeError) as exc:
        parser.exit(2, f"Invalid observation trace: {exc}\n")
    text = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
    else:
        print(text, end="")
    return 0 if result["observation_valid"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
