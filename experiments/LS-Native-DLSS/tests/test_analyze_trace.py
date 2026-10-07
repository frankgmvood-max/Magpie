import copy
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("trace", Path(__file__).parents[1] / "tools/analyze_trace.py")
trace = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trace)


def sample():
    textures = [dict(object=i + 10, width=3440, height=1440, format=28, view_format=28,
                     mip=0, array_size=1, samples=1, bind_flags=128) for i in range(6)]
    return [dict(kind="header", schema_version=1, dll_sha256=trace.DLL_HASH, shader_sha256=trace.SHADER_HASH,
                 resource_id=256, qpc_frequency=10000000, replacement_enabled=False,
                 present_owner="LS", source_frame_ids_known=False),
            dict(kind="dispatch", sequence=1, qpc=1000, device=1, context=2, adapter_luid=3, thread=4,
                 constants_known=True, phase_bits=0x3f000000, resolution_scale_bits=0x3f800000,
                 cb_byte_width=48, first_constant=0, num_constants=4096,
                 groups=[215, 90, 1], inputs=textures[:5], output=textures[5]),
            dict(kind="footer", written=1, seen=1, dropped_queue=0, dropped_busy=0, untagged=0,
                 tagged_shaders=1, tagged_buffers=1)]


class TraceTests(unittest.TestCase):
    def test_metadata_never_certifies_replacement_or_vrr(self):
        result = trace.analyze(sample())
        self.assertTrue(result["observation_valid"], result["errors"])
        self.assertFalse(result["replacement_verified"])
        self.assertFalse(result["physical_vrr_verified"])
        self.assertFalse(result["source_frame_ids_verified"])

    def test_corrupt_or_incomplete_trace(self):
        for mutate in [lambda x: x.pop(), lambda x: x[0].update(dll_sha256="wrong"),
                       lambda x: x[2].update(dropped_queue=1), lambda x: x[1].update(constants_known=False),
                       lambda x: x[1].update(sequence=2), lambda x: x[1].update(phase_bits=0x7fc00000),
                       lambda x: x[1].update(groups=[216, 90, 1]),
                       lambda x: x[1].update(cb_byte_width=32, first_constant=1)]:
            rows = sample()
            mutate(rows)
            self.assertFalse(trace.analyze(rows)["observation_valid"])

    def test_object_is_not_a_frame(self):
        rows = sample()
        rows[1]["inputs"][1] = copy.deepcopy(rows[1]["inputs"][0])
        result = trace.analyze(rows)
        self.assertTrue(result["warnings"])
        self.assertFalse(result["source_frame_ids_verified"])

    def test_cross_device_alias_and_repeated_sequence(self):
        rows = sample()
        row = copy.deepcopy(rows[1]); row.update(sequence=2, qpc=2000, device=20, adapter_luid=30)
        rows.insert(2, row); rows[-1].update(written=2, seen=2)
        self.assertFalse(trace.analyze(rows)["observation_valid"])
        rows[2].update(device=1, adapter_luid=3, sequence=1)
        self.assertFalse(trace.analyze(rows)["observation_valid"])

    def test_non_midpoint_has_no_dlss_authorization(self):
        rows = sample(); rows[1]["phase_bits"] = 0x3e800000
        result = trace.analyze(rows)
        self.assertTrue(result["observation_valid"])
        self.assertTrue(result["warnings"])
        self.assertFalse(result["replacement_verified"])

    def test_output_mip_dimensions(self):
        rows = sample(); rows[1]["output"]["mip"] = 1; rows[1]["groups"] = [108, 45, 1]
        self.assertTrue(trace.analyze(rows)["observation_valid"])


if __name__ == "__main__":
    unittest.main()
