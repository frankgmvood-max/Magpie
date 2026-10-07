#!/usr/bin/env python3
"""Read-only PE/DXBC inventory. Does not load or execute the input DLL.

Output contains metadata and hashes, never shader code or neural weights.
This is a parser, not an HLSL/CPU decompiler or a verified runtime pass graph.
"""
import argparse
import hashlib
import json
import struct
from collections import Counter
from pathlib import Path


class FormatError(ValueError):
    pass


class Reader:
    def __init__(self, data):
        self.data = data

    def bytes(self, offset, size):
        if offset < 0 or size < 0 or offset + size > len(self.data):
            raise FormatError(f"out-of-bounds read: {offset}+{size}")
        return self.data[offset:offset + size]

    def unpack(self, fmt, offset):
        return struct.unpack(fmt, self.bytes(offset, struct.calcsize(fmt)))

    def u32(self, offset):
        return self.unpack("<I", offset)[0]

    def string(self, offset):
        if not 0 <= offset < len(self.data):
            raise FormatError("string offset outside chunk")
        end = self.data.find(b"\0", offset)
        if end < 0:
            raise FormatError("unterminated string")
        return self.data[offset:end].decode("utf-8", errors="replace")


class PE(Reader):
    def __init__(self, data):
        super().__init__(data)
        if self.bytes(0, 2) != b"MZ":
            raise FormatError("not a PE file")
        nt = self.u32(0x3c)
        if self.bytes(nt, 4) != b"PE\0\0":
            raise FormatError("missing PE signature")
        self.machine, count = self.unpack("<HH", nt + 4)
        opt_size = self.unpack("<H", nt + 20)[0]
        opt = nt + 24
        magic = self.unpack("<H", opt)[0]
        if magic == 0x20b:
            self.image_base = self.unpack("<Q", opt + 24)[0]
            directory, number_offset = opt + 112, opt + 108
            self.pointer_size = 8
        elif magic == 0x10b:
            self.image_base = self.u32(opt + 28)
            directory, number_offset = opt + 96, opt + 92
            self.pointer_size = 4
        else:
            raise FormatError("unsupported PE optional header")
        self.header_size = self.u32(opt + 60)
        n = min(self.u32(number_offset), 16)
        if directory + n * 8 > opt + opt_size:
            raise FormatError("data directories exceed optional header")
        self.directories = [self.unpack("<II", directory + i * 8) for i in range(n)]
        self.sections = []
        for i in range(count):
            at = opt + opt_size + i * 40
            name = self.bytes(at, 8).split(b"\0")[0].decode(errors="replace")
            virtual_size, rva, raw_size, offset = self.unpack("<4I", at + 8)
            self.bytes(offset, raw_size)
            self.sections.append(dict(name=name, rva=rva, offset=offset,
                                      raw_size=raw_size, virtual_size=virtual_size,
                                      flags=self.u32(at + 36)))

    def directory(self, index):
        return self.directories[index] if index < len(self.directories) else (0, 0)

    def rva_offset(self, rva, size=1):
        if 0 <= rva < self.header_size and rva + size <= self.header_size:
            self.bytes(rva, size)
            return rva
        for s in self.sections:
            delta = rva - s["rva"]
            if 0 <= delta and delta + size <= s["raw_size"]:
                at = s["offset"] + delta
                self.bytes(at, size)
                return at
        raise FormatError(f"RVA 0x{rva:x} is not backed by file data")

    def offset_rva(self, offset):
        for s in self.sections:
            if s["offset"] <= offset < s["offset"] + s["raw_size"]:
                return s["rva"] + offset - s["offset"]
        raise FormatError("offset outside sections")

    def executable_rva(self, rva):
        return any(s["flags"] & 0x20000000 and s["rva"] <= rva <
                   s["rva"] + s["raw_size"] for s in self.sections)

    def resources(self):
        base_rva, size = self.directory(2)
        if not base_rva:
            return []
        root = self.rva_offset(base_rva, size)
        result = []

        def local(relative, length):
            if relative < 0 or relative + length > size:
                raise FormatError("resource directory exceeds its bounds")
            return root + relative

        def walk(relative, path, visited):
            if relative in visited or len(path) > 4:
                raise FormatError("cyclic or over-deep resource directory")
            at = local(relative, 16)
            named, ids = self.unpack("<HH", at + 12)
            local(relative + 16, (named + ids) * 8)
            for i in range(named + ids):
                key, target = self.unpack("<II", at + 16 + i * 8)
                if key & 0x80000000:
                    pos = key & 0x7fffffff
                    chars = self.unpack("<H", local(pos, 2))[0]
                    key = self.bytes(local(pos + 2, chars * 2), chars * 2).decode("utf-16le")
                branch = path + [key]
                pos = target & 0x7fffffff
                if target & 0x80000000:
                    walk(pos, branch, visited | {relative})
                else:
                    rva, length, codepage, _ = self.unpack("<4I", local(pos, 16))
                    result.append(dict(path=branch, rva=rva, size=length,
                                       offset=self.rva_offset(rva, length), codepage=codepage))
        walk(0, [], set())
        return result

    def exports(self):
        rva, size = self.directory(0)
        if not rva:
            return []
        at = self.rva_offset(rva, 40)
        base, count, named, funcs, names, ords = self.unpack("<6I", at + 16)
        fp, np, op = (self.rva_offset(funcs, count * 4),
                      self.rva_offset(names, named * 4), self.rva_offset(ords, named * 2))
        result = []
        for i in range(named):
            name = self.string(self.rva_offset(self.u32(np + i * 4)))
            ordinal_index = self.unpack("<H", op + i * 2)[0]
            if ordinal_index >= count:
                raise FormatError("bad export ordinal")
            fn = self.u32(fp + ordinal_index * 4)
            result.append(dict(name=name, ordinal=base + ordinal_index, rva=fn,
                               forwarded=rva <= fn < rva + size))
        return result

    def lsfg_vtables(self):
        """Candidate x64 MSVC RTTI vtables; method names/semantics remain unknown."""
        if self.machine != 0x8664:
            return []
        result = []
        for name in ("LSFG", "LSFG2", "LSFG3"):
            needle = (".?AV" + name + "@@\0").encode()
            pos = self.data.find(needle)
            if pos < 16:
                continue
            type_rva = self.offset_rva(pos - 16)
            for s in self.sections:
                if s["flags"] & 0x20000000:
                    continue
                start, end = s["offset"], s["offset"] + s["raw_size"]
                # TypeDescriptor RVA is the fourth word of an x64 COL.
                hits = start
                target = struct.pack("<I", type_rva)
                while True:
                    hits = self.data.find(target, hits, end)
                    if hits < 0:
                        break
                    col = hits - 12
                    hits += 4
                    if col < start or col + 24 > end or col % 4:
                        continue
                    signature, _, _, _, hierarchy, self_rva = self.unpack("<6I", col)
                    if signature != 1 or self_rva != self.offset_rva(col):
                        continue
                    try:
                        self.rva_offset(hierarchy, 16)
                    except FormatError:
                        continue
                    address = struct.pack("<Q", self.image_base + self_rva)
                    cursor = 0
                    while True:
                        cursor = self.data.find(address, cursor)
                        if cursor < 0:
                            break
                        vpos = cursor + 8
                        cursor += 8
                        if vpos % 8:
                            continue
                        methods = []
                        for i in range(64):
                            if vpos + (i + 1) * 8 > len(self.data):
                                break
                            fn = self.unpack("<Q", vpos + i * 8)[0] - self.image_base
                            if not self.executable_rva(fn):
                                break
                            methods.append(fn)
                        if methods:
                            result.append(dict(class_name=name, type_descriptor_rva=type_rva,
                                               locator_rva=self_rva, vtable_rva=self.offset_rva(vpos),
                                               method_rvas=methods, status="RTTI_candidate_not_semantic_mapping"))
        return result


BIND_TYPES = {0: "cbuffer", 1: "tbuffer", 2: "texture", 3: "sampler",
              4: "typed_uav", 5: "structured_srv", 6: "structured_uav",
              7: "byte_address_srv", 8: "byte_address_uav"}
PROGRAM_TYPES = {0: "pixel", 1: "vertex", 2: "geometry", 3: "hull", 4: "domain", 5: "compute"}


def reflection(data):
    d = Reader(data)
    cb_count, cb_off, bind_count, bind_off, target = d.unpack("<5I", 0)
    major, minor = (target >> 8) & 0xff, target & 0xff
    if major != 5 or minor not in (0, 1):
        raise FormatError(f"unsupported RDEF target {major}.{minor}")
    bind_stride = 40 if minor == 1 else 32
    bindings = []
    for i in range(bind_count):
        v = d.unpack("<8I", bind_off + i * bind_stride)
        item = dict(name=d.string(v[0]), type=BIND_TYPES.get(v[1], f"type_{v[1]}"),
                    type_id=v[1], return_type=v[2], dimension=v[3],
                    slot=v[5], count=v[6], flags=v[7])
        if minor == 1:
            item["space"], item["id"] = d.unpack("<II", bind_off + i * bind_stride + 32)
        bindings.append(item)
    buffers = []
    for i in range(cb_count):
        v = d.unpack("<6I", cb_off + i * 24)
        variables = []
        for k in range(v[1]):
            a = d.unpack("<10I", v[2] + k * 40)
            cls, typ, rows, columns, elements, members, member_off = d.unpack("<6HI", a[4])
            variables.append(dict(name=d.string(a[0]), offset=a[1], size=a[2],
                                  flags=a[3], type_class=cls, type_id=typ, rows=rows,
                                  columns=columns, elements=elements, members=members))
        buffers.append(dict(name=d.string(v[0]), size=v[3], flags=v[4], variables=variables))
    return dict(bindings=bindings, constant_buffers=buffers,
                compiler=d.string(d.u32(24)), shader_model=f"{major}.{minor}")


def program(data):
    d = Reader(data)
    version, words = d.unpack("<II", 0)
    if words * 4 != len(data):
        raise FormatError("shader token length mismatch")
    tokens = d.unpack("<" + "I" * words, 0)
    index, counts, group = 2, Counter(), None
    while index < words:
        token = tokens[index]
        opcode = token & 0x7ff
        length = (token >> 24) & 0x7f
        if opcode == 53:  # CUSTOMDATA length follows its header token.
            if index + 1 >= words:
                raise FormatError("truncated CUSTOMDATA")
            length = tokens[index + 1]
        if length < 1 or index + length > words:
            raise FormatError(f"invalid instruction length at token {index}")
        counts[opcode] += 1
        if opcode == 155:  # DCL_THREAD_GROUP, per Microsoft's tokenized format.
            if length != 4:
                raise FormatError("invalid thread group declaration")
            group = list(tokens[index + 1:index + 4])
        index += length
    return dict(stage=PROGRAM_TYPES.get(version >> 16, "unknown"),
                shader_model=f"{(version >> 4) & 15}.{version & 15}",
                thread_group=group, instruction_records=sum(counts.values()),
                opcode_counts={str(k): v for k, v in sorted(counts.items())})


def dxbc(data):
    d = Reader(data)
    if d.bytes(0, 4) != b"DXBC":
        raise FormatError("not a DXBC container")
    size, count = d.unpack("<II", 24)
    if size != len(data):
        raise FormatError("DXBC size mismatch")
    d.bytes(32, count * 4)
    chunks, intervals, result = [], [], {}
    for i in range(count):
        at = d.u32(32 + i * 4)
        if at < 32 + count * 4:
            raise FormatError("chunk overlaps DXBC header")
        tag = d.bytes(at, 4).decode("ascii")
        length = d.u32(at + 4)
        chunk = d.bytes(at + 8, length)
        end = at + 8 + length
        if any(at < b and a < end for a, b in intervals):
            raise FormatError("overlapping DXBC chunks")
        intervals.append((at, end))
        chunks.append(tag)
        if tag == "RDEF":
            result["reflection"] = reflection(chunk)
        elif tag in ("SHEX", "SHDR"):
            result["program"] = program(chunk)
    result.update(size=size, sha256=hashlib.sha256(data).hexdigest(), chunks=chunks)
    return result


def analyze(data):
    pe = PE(data)
    resources = pe.resources()
    shaders, errors = [], []
    for resource in resources:
        payload = pe.bytes(resource["offset"], resource["size"])
        if not payload.startswith(b"DXBC"):
            continue
        try:
            shader = dxbc(payload)
            shader.update(resource_path=resource["path"], offset=resource["offset"],
                          rva=resource["rva"], semantic_role="unverified")
            shaders.append(shader)
        except (FormatError, UnicodeDecodeError) as exc:
            errors.append(dict(resource_path=resource["path"], error=str(exc)))
    return dict(schema_version=1, input_size=len(data), input_sha256=hashlib.sha256(data).hexdigest(),
                machine=hex(pe.machine), image_base=pe.image_base, sections=pe.sections,
                resource_count=len(resources), shaders=shaders, errors=errors,
                exports=pe.exports(), lsfg_vtables=pe.lsfg_vtables(),
                limitations=["Binding names are not proof of a pass's runtime role.",
                             "No source frame IDs, interpolation phase or ordering are inferred.",
                             "No CPU/HLSL source or proprietary shader code is emitted."])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("dll", type=Path)
    ap.add_argument("--output", type=Path, required=True)
    args = ap.parse_args()
    result = analyze(args.dll.read_bytes())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"{len(result['shaders'])} shaders, {result['resource_count']} resources, "
          f"{len(result['errors'])} errors, {len(result['lsfg_vtables'])} RTTI vtable candidates")
    return bool(result["errors"])


if __name__ == "__main__":
    raise SystemExit(main())
