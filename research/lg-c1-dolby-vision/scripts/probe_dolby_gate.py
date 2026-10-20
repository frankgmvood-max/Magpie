#!/usr/bin/env python3
"""Emulate isolated ARM leaf functions; no firmware services or hardware run."""

import argparse
import hashlib
import itertools
import json
from pathlib import Path
import struct

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP, UC_ARM_REG_LR

WORK, STACK, STOP = 0x2000000, 0x3000000, 0x4000000


class LeafProbe:
    def __init__(self, path):
        self.data = path.read_bytes()
        self.segments = []
        self.symbols = []
        self.emulator_patch = None
        with path.open("rb") as source:
            elf = ELFFile(source)
            if elf["e_machine"] != "EM_ARM" or elf.elfclass != 32:
                raise ValueError("This probe currently supports ARM32 only")
            for segment in elf.iter_segments():
                if segment["p_type"] == "PT_LOAD":
                    self.segments.append((segment["p_vaddr"], segment["p_memsz"], segment.data()))
            for symbol in elf.get_section_by_name(".dynsym").iter_symbols():
                if symbol["st_info"]["type"] == "STT_FUNC" and symbol["st_size"] and symbol["st_shndx"] != "SHN_UNDEF":
                    self.symbols.append((symbol.name, symbol["st_value"], symbol["st_size"]))

    def find(self, fragment):
        matches = [s for s in self.symbols if fragment in s[0]]
        if len(matches) != 1:
            raise ValueError(f"Expected exactly one symbol matching {fragment!r}, found {len(matches)}")
        return matches[0]

    def run(self, symbol, selector, avi=None, capture_string=False, timing=None):
        name, entry, size = symbol
        start = entry & ~1
        if not entry & 1:
            raise ValueError("Expected a Thumb function")
        engine = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        end = max(v + n for v, n, _ in self.segments)
        engine.mem_map(0, (end + 0xfff) & ~0xfff)
        for address, _, data in self.segments:
            engine.mem_write(address, data)
        if self.emulator_patch is not None:
            address, before, after = self.emulator_patch
            if bytes(engine.mem_read(address, len(before))) != before:
                raise ValueError("Emulator patch preimage mismatch")
            engine.mem_write(address, after)
        engine.mem_map(WORK, 0x10000)
        engine.mem_map(STACK, 0x10000)
        engine.mem_map(STOP, 0x1000)
        engine.reg_write(UC_ARM_REG_SP, STACK + 0xff00)
        engine.reg_write(UC_ARM_REG_LR, STOP | 1)
        engine.reg_write(UC_ARM_REG_R0, WORK)
        engine.reg_write(UC_ARM_REG_R1, 0 if capture_string else selector)
        engine.reg_write(UC_ARM_REG_R2, selector if capture_string else WORK + 0x1000)
        engine.reg_write(UC_ARM_REG_R3, WORK + 0x2000)
        if avi is not None:
            for offset, value in zip((8, 0x34, 0x38), avi):
                engine.mem_write(WORK + 0x1000 + offset, struct.pack("<I", value))
        if timing is not None:
            engine.mem_write(WORK + 0x2000 + 0x14, struct.pack("<HH", *timing))
        result = {}

        def hook(machine, address, instruction_size, user_data):
            if address == STOP:
                result["value"] = machine.reg_read(UC_ARM_REG_R0)
                machine.emu_stop()
            elif not start <= address < start + size:
                if not capture_string:
                    raise RuntimeError(f"Unexpected call out of leaf function: {address:#x}")
                pointer = machine.reg_read(UC_ARM_REG_R1)
                raw = bytes(machine.mem_read(pointer, 128)).split(b"\0", 1)[0]
                result["value"] = raw.decode("ascii")
                result["string_address"] = hex(pointer)
                machine.emu_stop()

        engine.hook_add(UC_HOOK_CODE, hook)
        engine.emu_start(entry, STOP + 2, count=5000)
        if "value" not in result:
            raise RuntimeError(f"No return/capture from {name}")
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    experiments = parser.add_mutually_exclusive_group()
    experiments.add_argument("--experiment-c1-rgb-full", action="store_true",
                             help="Replace Limited with Full in emulator memory; demonstrates a regression")
    experiments.add_argument("--experiment-c1-rgb-both", action="store_true",
                             help="Accept Limited and Full in emulator memory only; never writes a patched binary")
    args = parser.parse_args()
    probe = LeafProbe(args.library)
    gate = probe.find("is_dolby_valid")
    c1_signature = "_ZN27ruleDolbyVisionSignalDetect14is_dolby_validE28v4l2_ext_hdmi_dolby_hdr_typeR22v4l2_ext_hdmi_avi_info"
    c3_signature = c1_signature + "R25v4l2_ext_hdmi_timing_info"
    if gate[0] not in (c1_signature, c3_signature):
        raise ValueError("Unknown predicate ABI; inspect the signature before emulation")
    has_timing = gate[0] == c3_signature
    digest = hashlib.sha256(probe.data).hexdigest()
    mutation = None
    if args.experiment_c1_rgb_full or args.experiment_c1_rgb_both:
        expected = "f6b8e5c055b41f18695f0b611c36023d0dfee75e7de445b15f6b2b1340ff039d"
        if digest != expected or has_timing:
            raise ValueError("Experiment is pinned to the analyzed C1 03.53.45 library")
        if args.experiment_c1_rgb_full:
            mutation = {"elf_vma": "0x3e59e", "before": "a0f10100", "after": "a0f10200",
                        "meaning": "Compare LLDV RGB range to 2 instead of 1; also removes acceptance of range 1"}
        else:
            mutation = {"elf_vma": "0x3e59c", "before": "506ba0f10100b0fa80f040097047",
                        "after": "506b0138012894bf012000207047",
                        "meaning": "Accept LLDV RGB range 1 or 2; retain the other branches unchanged"}
        probe.emulator_patch = (int(mutation["elf_vma"], 16), bytes.fromhex(mutation["before"]), bytes.fromhex(mutation["after"]))
    label_functions = {
        "dolby_type": "get_dolbyhdr_type_stringB5cxx11E28",
        "pixel_encoding": "get_pixel_encoding_stringB5cxx11E21",
        "rgb_range": "get_rgb_quantization_range_stringB5cxx11E40",
        "ycc_range": "get_ycbcr_quantization_range_stringB5cxx11E40",
    }
    labels = {}
    for field, fragment in label_functions.items():
        # Pixel-encoding type name length varies between firmware revisions.
        matches = [s for s in probe.symbols if fragment in s[0]]
        if field == "pixel_encoding" and not matches:
            matches = [s for s in probe.symbols if "get_pixel_encoding_stringB5cxx11E" in s[0] and not s[0].endswith("Ev")]
        if len(matches) != 1:
            raise ValueError(f"Cannot uniquely resolve label formatter for {field}")
        labels[field] = {str(i): probe.run(matches[0], i, capture_string=True) for i in range(4)}
    rows = []
    timing_cases = [(3840, 2160), (4096, 2160)] if has_timing else [(3840, 2160)]
    for timing in timing_cases:
        for dolby, encoding, rgb, ycc in itertools.product(range(4), range(4), range(4), range(3)):
            actual = probe.run(gate, dolby, (encoding, rgb, ycc), timing=timing)["value"]
            rows.append({"dolby_type": dolby, "pixel_encoding": encoding,
                         "rgb_range": rgb, "ycc_range": ycc,
                         "timing_u16_0x14": timing[0], "timing_u16_0x16": timing[1],
                         "accepted": bool(actual)})
    report = {
        "library": args.library.name,
        "sha256": digest,
        "predicate_symbol": gate[0], "predicate_elf_vma": hex(gate[1] & ~1),
        "predicate_size": gate[2], "labels": labels,
        "timing_argument_present": has_timing,
        "emulator_only_mutation": mutation,
        "scope": "Isolated predicate and label formatters under Unicorn; not an HDMI or TV test.",
        "cases": rows,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Executed {len(rows)} predicate cases; wrote {args.output}")


if __name__ == "__main__":
    main()
