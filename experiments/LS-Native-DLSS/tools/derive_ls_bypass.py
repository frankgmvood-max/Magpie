#!/usr/bin/env python3
"""Derive the published call contract from genuine private REA bridge responses.

Publishes only our address/resource observations, never native disassembly or
pseudocode. Static evidence does not prove GPU/runtime acceptance.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path

TARGET = '626b196d799606cd4250b7b29e04228692ab70cf56a5d1bbb56d748c8219f0eb'
BASE = 0x180000000

def derive(folder):
    raw = (folder / 'requests.jsonl.results').read_bytes()
    provenance = json.loads((folder / 'provenance.json').read_text())
    assert provenance['rea'] == '6.3.0' and provenance['ghidra'] == '12.1.4'
    assert provenance['responses_sha256'] == hashlib.sha256(raw).hexdigest()
    rows = [json.loads(x) for x in raw.decode().splitlines() if x.strip()]
    assert len(rows) == 23 and all(x['ok'] for x in rows)
    result = {x['id']: x['result'] for x in rows}
    assert result[1]['analysis_complete'] and not result[1]['analysis_timed_out']
    assert result[1]['target']['sha256'] == TARGET and result[2]['source_files'][0]['original_sha256'] == TARGET
    assert result[2]['source_files'][0]['modified_sha256'] == TARGET
    slots = {}
    for resource, slot in re.findall(r'FUN_180028730\([^\n]*?,(0x[0-9a-f]+|\d+),param_1 \+ (0x[0-9a-f]+)\)', result[13]):
        slots.setdefault(int(slot, 0), []).append(int(resource, 0))
    slots[0x11e8] = [0x117, 0x12e]
    calls = []
    for row, name, count in ((5, 'source', 6), (8, 'prepare', 5), (11, 'synthesis', 16)):
        instructions = result[row]['instructions']; selected = []
        for n, instruction in enumerate(instructions):
            if 'CALL qword ptr [RAX + 0x148]' not in instruction:
                continue
            preceding = instructions[max(0,n-150):n]
            binds = [i for i,s in enumerate(preceding) if 'CALL qword ptr [RAX + 0x228]' in s]
            assert binds
            last = preceding[max(0,binds[-1]-5):binds[-1]]
            matches = [re.search(r'MOV RDX, qword ptr \[\w+ \+ (0x[0-9a-f]+)\]', x) for x in last]
            offset = int(next(m.group(1) for m in matches if m),16)
            call_rva = int(instruction.split(':')[0],16)-BASE
            return_rva = int(instructions[n+1].split(':')[0],16)-BASE
            role = 'keep_source' if offset == 0x1128 else 'replace_synthesis' if offset == 0x11f0 else 'bypass_analysis'
            assert return_rva == call_rva+6
            selected.append(dict(function=name,call_rva=call_rva,return_rva=return_rva,
                                 role=role,shader_slot=offset,resource_ids=slots[offset]))
        assert len(selected) == count
        calls.extend(selected)
    assert sum(c['role']=='bypass_analysis' for c in calls) == 25
    return dict(schema='ls-rea-dispatch-contract-v1',target_sha256=TARGET,rea='6.3.0',ghidra='12.1.4',
                adapter=provenance['adapter'],bridge_sha256=provenance['bridge_sha256'],
                responses_sha256=provenance['responses_sha256'],request_count=23,
                analysis_complete=True,static_only=True,upstream_mcp_ledger=False,calls=calls)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('folder',type=Path);p.add_argument('output',type=Path)
    a=p.parse_args();data=derive(a.folder);a.output.write_text(json.dumps(data,indent=2)+'\n')
    print('Verified actual REA responses; published 27 call observations, 25 analysis returns.')
if __name__=='__main__': main()
