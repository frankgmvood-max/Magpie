import importlib.util
import json
import struct
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location("analyzer", Path(__file__).parents[1] / "tools/analyze_dll.py")
analyzer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analyzer)


def container(chunks):
    header = 32 + 4 * len(chunks)
    positions, payload = [], b""
    for tag, data in chunks:
        positions.append(header + len(payload))
        payload += tag + struct.pack("<I", len(data)) + data
    return b"DXBC" + bytes(16) + struct.pack("<III", 1, header + len(payload), len(chunks)) + \
        struct.pack("<" + "I" * len(chunks), *positions) + payload


class ParserTests(unittest.TestCase):
    def test_thread_group(self):
        data = struct.pack("<6I", 0x50050, 6, (4 << 24) | 155, 8, 8, 1)
        p = analyzer.dxbc(container([(b"SHEX", data)]))["program"]
        self.assertEqual(p["stage"], "compute")
        self.assertEqual(p["thread_group"], [8, 8, 1])
        self.assertEqual(p, json.loads(json.dumps(p)))

    def test_rejects_zero_length_instruction(self):
        with self.assertRaises(analyzer.FormatError):
            analyzer.program(struct.pack("<3I", 0x50050, 3, 0))

    def test_rejects_truncated_chunk(self):
        with self.assertRaises(analyzer.FormatError):
            analyzer.dxbc(container([(b"SHEX", bytes(8))])[:-1])

    def test_rejects_chunk_overlapping_header(self):
        p = bytearray(container([(b"STAT", bytes(4))]))
        struct.pack_into("<I", p, 32, 0)
        with self.assertRaises(analyzer.FormatError):
            analyzer.dxbc(bytes(p))

    def test_customdata_length(self):
        p = analyzer.program(struct.pack("<6I", 0x50050, 6, 53, 4, 1, 2))
        self.assertEqual(p["instruction_records"], 1)

    def test_bad_pe(self):
        for data in (b"", b"MZ" + bytes(62), b"not a DLL"):
            with self.assertRaises(analyzer.FormatError):
                analyzer.PE(data)


if __name__ == "__main__":
    unittest.main()
