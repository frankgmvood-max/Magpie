import json
import re
import unittest
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
class ContractTests(unittest.TestCase):
    def test_reviewed_returns_match_actual_rea_derived_contract(self):
        c=json.loads((ROOT/'analysis/ls-dispatch-contract.json').read_text())
        self.assertEqual(c['target_sha256'],'626b196d799606cd4250b7b29e04228692ab70cf56a5d1bbb56d748c8219f0eb')
        self.assertTrue(c['analysis_complete'])
        self.assertTrue(c['static_only'])
        self.assertFalse(c['upstream_mcp_ledger'])
        header=(ROOT/'include/ls_dispatch_contract.h').read_text()
        array=header.split('kAnalysisReturns{{',1)[1].split('}};',1)[0]
        actual=[int(x,16) for x in re.findall(r'0x[0-9a-f]+',array)]
        expected=[x['return_rva'] for x in c['calls'] if x['role']=='bypass_analysis']
        self.assertEqual(actual,expected)
        self.assertEqual(len(actual),25)
        self.assertEqual(len(set(actual)),25)
        for item in c['calls']:
            self.assertEqual(item['return_rva'],item['call_rva']+6)
            self.assertTrue(item['resource_ids'])
        source=[x for x in c['calls'] if x['role']=='keep_source']
        final=[x for x in c['calls'] if x['role']=='replace_synthesis']
        self.assertEqual([x['resource_ids'] for x in source],[[254]])
        self.assertEqual([x['resource_ids'] for x in final],[[256]])
        self.assertNotIn(source[0]['return_rva'],actual)
        self.assertNotIn(final[0]['return_rva'],actual)
        self.assertEqual(source[0]['return_rva'],0x20399)
        self.assertEqual(final[0]['return_rva'],0x245d3)

if __name__=='__main__':unittest.main()
