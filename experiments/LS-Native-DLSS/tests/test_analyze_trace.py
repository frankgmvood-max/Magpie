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


def sample_v2():
    rows = sample()
    rows[0].update(schema_version=2, input_shader_sha256=trace.INPUT_SHADER_HASH, input_resource_id=254,
                   source_mutation_coverage_complete=False, source_content_verified=False)
    synthesis = rows[1]
    writes = []
    for index in range(2):
        output = copy.deepcopy(synthesis["inputs"][index])
        source = copy.deepcopy(output); source["object"] = 100
        write = {k: synthesis[k] for k in ("device", "context", "adapter_luid", "thread")}
        write.update(epoch=200, generation=index + 1, sequence=index + 1,
                     qpc=500 + index * 100, command_epoch=0, texture=output)
        event = copy.deepcopy(synthesis)
        event.update(kind="input_update", sequence=write["sequence"], qpc=write["qpc"],
                     inputs=[source], output=output, groups=[430, 180, 1], source_write=write)
        writes.append(event)
    synthesis.update(sequence=3, pair_matches_observed_updates=True,
                     previous_write=copy.deepcopy(writes[0]["source_write"]),
                     current_write=copy.deepcopy(writes[1]["source_write"]))
    rows[1:1] = writes
    rows[-1].update(written=3, seen=3, tagged_shaders=2)
    return rows


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

    def test_observed_source_pair_does_not_certify_pixels_or_replacement(self):
        result = trace.analyze(sample_v2())
        self.assertTrue(result["observation_valid"], result["errors"])
        self.assertEqual(result["pairs_matching_observed_updates"], 1)
        self.assertEqual(result["input_updates"], 2)
        self.assertFalse(result["source_content_verified"])
        self.assertFalse(result["replacement_verified"])
        self.assertFalse(result["physical_vrr_verified"])

    def test_source_pair_corruption(self):
        for mutate in [lambda x: x[0].update(source_content_verified=True),
                       lambda x: x[0].update(input_shader_sha256="wrong"),
                       lambda x: x[1]["source_write"].update(generation=2),
                       lambda x: x[2]["source_write"].update(generation=4),
                       lambda x: x[2]["source_write"].update(qpc=400),
                       lambda x: x[2].update(groups=[215, 90, 1]),
                       lambda x: x[3].update(previous_write=x[3]["current_write"]),
                       lambda x: x[3]["inputs"].reverse(),
                       lambda x: x[3].update(qpc=550),
                       lambda x: x[3].update(pair_matches_observed_updates=1)]:
            rows = sample_v2(); mutate(rows)
            self.assertFalse(trace.analyze(rows)["observation_valid"], mutate)

    def test_missing_pair_is_native_fallback(self):
        rows = sample_v2(); rows[3].update(pair_matches_observed_updates=False)
        result = trace.analyze(rows)
        self.assertTrue(result["observation_valid"], result["errors"])
        self.assertEqual(result["pairs_matching_observed_updates"], 0)
        self.assertTrue(result["warnings"])

    def test_reused_texture_and_multiple_slots(self):
        rows = sample_v2()
        third = copy.deepcopy(rows[1])
        third.update(sequence=4, qpc=1100)
        third["source_write"].update(sequence=4, qpc=1100, generation=3)
        synthesis = copy.deepcopy(rows[3])
        synthesis.update(sequence=5, qpc=1200, previous_write=copy.deepcopy(rows[2]["source_write"]),
                         current_write=copy.deepcopy(third["source_write"]))
        synthesis["inputs"][0], synthesis["inputs"][1] = synthesis["inputs"][1], synthesis["inputs"][0]
        repeated = copy.deepcopy(synthesis); repeated.update(sequence=6, qpc=1300)
        rows[-1:-1] = [third, synthesis, repeated]
        rows[-1].update(written=6, seen=6)
        result = trace.analyze(rows)
        self.assertTrue(result["observation_valid"], result["errors"])
        self.assertEqual(result["pairs_matching_observed_updates"], 3)

    def test_new_source_epoch_after_unknown_write(self):
        rows = sample_v2()
        updates = copy.deepcopy(rows[1:3])
        for index, update in enumerate(updates):
            update.update(sequence=4 + index, qpc=1100 + index * 100)
            update["source_write"].update(epoch=201, sequence=update["sequence"], qpc=update["qpc"])
        synthesis = copy.deepcopy(rows[3]); synthesis.update(sequence=6, qpc=1300,
            previous_write=updates[0]["source_write"], current_write=updates[1]["source_write"])
        rows[-1:-1] = [*updates, synthesis]; rows[-1].update(written=6, seen=6)
        self.assertTrue(trace.analyze(rows)["observation_valid"])
        rows[-2]["current_write"] = rows[2]["source_write"]
        self.assertFalse(trace.analyze(rows)["observation_valid"])


if __name__ == "__main__":
    unittest.main()
