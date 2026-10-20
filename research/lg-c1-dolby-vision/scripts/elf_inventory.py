#!/usr/bin/env python3
"""Read-only ELF inventory and ARM disassembly; never loads target libraries."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB
from elftools.elf.elffile import ELFFile


def inspect(path, pattern, disassemble):
    data = path.read_bytes()
    with path.open("rb") as source:
        elf = ELFFile(source)
        symbols = []
        seen = set()
        for section_name in (".dynsym", ".symtab"):
            section = elf.get_section_by_name(section_name)
            if section is None:
                continue
            for symbol in section.iter_symbols():
                name = symbol.name
                if not pattern.search(name) or symbol["st_shndx"] == "SHN_UNDEF":
                    continue
                key = (name, symbol["st_value"])
                if key in seen:
                    continue
                seen.add(key)
                record = {
                    "symbol": name,
                    "address": hex(symbol["st_value"] & ~1),
                    "size": symbol["st_size"],
                    "type": symbol["st_info"]["type"],
                }
                if disassemble and record["type"] == "STT_FUNC" and elf["e_machine"] == "EM_ARM":
                    address = symbol["st_value"] & ~1
                    mode = CS_MODE_THUMB if symbol["st_value"] & 1 else CS_MODE_ARM
                    for segment in elf.iter_segments():
                        start = segment["p_vaddr"]
                        if segment["p_type"] == "PT_LOAD" and start <= address < start + segment["p_filesz"]:
                            offset = segment["p_offset"] + address - start
                            code = data[offset:offset + symbol["st_size"]]
                            engine = Cs(CS_ARCH_ARM, mode)
                            record["instructions"] = [
                                {"address": hex(i.address), "bytes": i.bytes.hex(),
                                 "mnemonic": i.mnemonic, "operands": i.op_str}
                                for i in engine.disasm(code, address)
                            ]
                            break
                symbols.append(record)
        if symbols:
            names = "\n".join(s["symbol"] for s in symbols) + "\n"
            demangled = subprocess.run(["c++filt"], input=names, text=True,
                                       check=True, capture_output=True).stdout.splitlines()
            for symbol, name in zip(symbols, demangled):
                symbol["demangled"] = name
        return {
            "filename": path.name,
            "sha256": hashlib.sha256(data).hexdigest(),
            "bytes": len(data), "elf_class": elf.elfclass,
            "machine": elf["e_machine"], "symbols": symbols,
            "matching_strings": [
                {"file_offset": hex(match.start()), "text": match.group().decode("ascii")}
                for match in re.finditer(rb"[\x20-\x7e]{5,}", data)
                if pattern.search(match.group().decode("ascii"))
            ],
        }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="+", type=Path)
    parser.add_argument("--pattern", default=r"dolby|dovi|vsvdb|enableDolbyVisionPC|hdr.*support")
    parser.add_argument("--disassemble", action="store_true")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    result = [inspect(p, re.compile(args.pattern, re.I), args.disassemble) for p in args.files]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
    print(f"Inventoried {len(result)} files into {args.output}")


if __name__ == "__main__":
    main()
