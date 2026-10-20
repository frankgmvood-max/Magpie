#!/usr/bin/env python3
"""Run the C3 PC-Dolby detector on synthetic state in Unicorn; no TV access."""

import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_LR

from probe_dolby_gate import LeafProbe, WORK, STACK, STOP

EXPECTED_SHA256 = "040fd267b220fc7f0c4ad2272218b6d1f1cb5cbdab4583013e82824a04c507db"
GET_BOOL_PLT = 0x16e7c
# Offsets relative to v4l2_ext_hdmi_drm_info, confirmed by print_drm_packet strings.
SIGNATURE = {
    0x10: ("display_primaries_x0", 0x8a26, 0xfffe),
    0x12: ("display_primaries_y0", 0x3976, 0xfffe),
    0x14: ("display_primaries_x1", 0x2108, 0xfffe),
    0x16: ("display_primaries_y1", 0x9b7e, 0xfffe),
    0x18: ("display_primaries_x2", 0x19fa, 0xfffe),
    0x1a: ("display_primaries_y2", 0x0846, 0xfffe),
    0x1c: ("white_point_x", 0x00ee, 0x00fe),
    0x1e: ("white_point_y", 0x0030, 0x00fe),
    0x24: ("maximum_content_light_level", 0x00fe, 0x00fe),
    0x26: ("maximum_frame_average_light_level", 0x00aa, 0x00fe),
}


class DetectorProbe:
    def __init__(self, library):
        self.binary = LeafProbe(library)
        self.digest = hashlib.sha256(self.binary.data).hexdigest()
        if self.digest != EXPECTED_SHA256:
            raise ValueError("Only the analyzed C3 23.25.55 library is supported")
        self.detector = self.binary.find("is_dolby_pc_supported")
        self.get_bool = self.binary.find("getBoolValue")

    def run(self, state):
        engine = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        end = max(v + n for v, n, _ in self.binary.segments)
        engine.mem_map(0, (end + 0xfff) & ~0xfff)
        for address, _, data in self.binary.segments:
            engine.mem_write(address, data)
        engine.mem_map(WORK, 0x10000)
        engine.mem_map(STACK, 0x10000)
        engine.mem_map(STOP, 0x1000)
        control, info, timing, config = (WORK + i * 0x1000 for i in range(1, 5))

        def put(address, value, fmt="I"):
            engine.mem_write(address, struct.pack("<" + fmt, value))

        put(WORK + 0x5c, control)
        for offset, address in ((0x140, info), (0x138, timing), (0x174, config)):
            put(control + offset, address)
        put(config + 0x30, state["support_dolby"], "B")
        put(timing + 0x1c, state["timing_u16_0x1c"], "H")
        put(timing + 0x1e, state["timing_u16_0x1e"], "H")
        put(info + 0x18, state["pixel_encoding"])
        put(info + 0x44, state["rgb_range"])
        # set_drm_infoframe copies the 0x28-byte driver structure to info+0x204.
        drm = info + 0x204
        put(drm + 4, state["drm_version"], "B")
        put(drm + 8, state["eotf_type"])
        for offset, value in state["drm_u16"].items():
            put(drm + int(offset, 16), value, "H")
        engine.reg_write(UC_ARM_REG_SP, STACK + 0xff00)
        engine.reg_write(UC_ARM_REG_LR, STOP | 1)
        engine.reg_write(UC_ARM_REG_R0, WORK)
        result = {}
        allowed = [(s[1] & ~1, (s[1] & ~1) + s[2]) for s in (self.detector, self.get_bool)]

        def hook(machine, address, size, user_data):
            if address == STOP:
                result["accepted"] = bool(machine.reg_read(UC_ARM_REG_R0))
                machine.emu_stop()
            elif address == GET_BOOL_PLT:
                # Resolve this one local PLT call to the original function.
                # Neither the detector nor its dependency's return value is stubbed.
                machine.reg_write(UC_ARM_REG_PC, self.get_bool[1])
            elif not any(start <= address < end for start, end in allowed):
                raise RuntimeError(f"Unexpected execution outside detector/dependency: {address:#x}")

        engine.hook_add(UC_HOOK_CODE, hook)
        engine.emu_start(self.detector[1], STOP + 2, count=10000)
        if "accepted" not in result:
            raise RuntimeError("Detector did not return")
        return result["accepted"]


def cases():
    base = {
        "support_dolby": 1, "drm_version": 1, "eotf_type": 2,
        "timing_u16_0x1c": 3840, "timing_u16_0x1e": 2160,
        "pixel_encoding": 0, "rgb_range": 2,
        "drm_u16": {hex(o): v for o, (_, v, _) in SIGNATURE.items()} | {"0x20": 1},
    }
    yield "matching_signature", base, True
    for field, value in (("support_dolby", 0), ("drm_version", 0), ("drm_version", 2),
                         ("eotf_type", 0), ("eotf_type", 3)):
        state = copy.deepcopy(base)
        state[field] = value
        yield f"{field}_{value}", state, False
    state = copy.deepcopy(base)
    state["drm_u16"]["0x20"] = 0
    yield "mastering_luminance_zero", state, False
    state = copy.deepcopy(base)
    state["drm_u16"] = {key: 0 for key in state["drm_u16"]}
    state["drm_u16"]["0x20"] = 1000
    yield "hdr_without_signature", state, False
    for offset, (name, value, mask) in SIGNATURE.items():
        for variant, replacement, expected in (
            ("lsb_ignored", value ^ 1, True),
            ("checked_bit_changed", value ^ 2, False),
            ("high_byte_changed", value ^ 0x100, mask == 0x00fe),
        ):
            state = copy.deepcopy(base)
            state["drm_u16"][hex(offset)] = replacement
            yield f"{name}_{variant}", state, expected
    for encoding in range(4):
        for rgb_range in range(4):
            state = copy.deepcopy(base)
            state.update(pixel_encoding=encoding, rgb_range=rgb_range)
            yield f"encoding_{encoding}_rgb_range_{rgb_range}", state, encoding != 0 or rgb_range == 2
    for first, second, expected in ((4096, 2160, False), (4096, 2159, True),
                                    (3840, 2161, True), (3841, 2161, False), (1920, 1080, True)):
        state = copy.deepcopy(base)
        state.update(timing_u16_0x1c=first, timing_u16_0x1e=second)
        yield f"timing_{first}_{second}", state, expected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("library", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    probe = DetectorProbe(args.library)
    results = []
    for name, state, expected in cases():
        accepted = probe.run(state)
        if accepted != expected:
            raise AssertionError(f"{name}: expected {expected}, got {accepted}")
        results.append({"name": name, "state": state, "accepted": accepted})
    hdr_formatter = [s for s in probe.binary.symbols
                     if "get_hdr_type_stringB5cxx11E" in s[0] and not s[0].endswith("Ev")]
    if len(hdr_formatter) != 1:
        raise ValueError("Cannot uniquely resolve HDR label formatter")
    hdr_labels = {str(i): probe.binary.run(hdr_formatter[0], i, capture_string=True)["value"]
                  for i in range(7)}
    report = {
        "sha256": probe.digest,
        "detector_symbol": probe.detector[0],
        "detector_elf_vma": hex(probe.detector[1] & ~1),
        "dependency_elf_vma": hex(probe.get_bool[1] & ~1),
        "plt_resolution": {hex(GET_BOOL_PLT): hex(probe.get_bool[1])},
        "hdr_type_labels_from_original_formatter": hdr_labels,
        "scope": "Original detector, getBoolValue, and HDR label formatter on synthetic memory. No driver, HDMI source, TV, or patched binary.",
        "signature": [{"drm_offset": hex(o), "field": name, "value": hex(v), "mask": hex(mask)}
                      for o, (name, v, mask) in SIGNATURE.items()],
        "cases": results,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"Executed {len(results)} PC detector cases; wrote {args.output}")


if __name__ == "__main__":
    main()
