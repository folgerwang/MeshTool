import copy
import json
import hashlib
import tempfile
import unittest
from pathlib import Path
from rules_config import RULES,configure,snapshot
from building_rules import opening_template


class RulesConfigTest(unittest.TestCase):
    def tearDown(self):configure()

    def test_custom_config_changes_real_geometry_and_is_snapshotted(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'custom.json'
            path.write_text(json.dumps({'windows':{'recess_m':.22}}))
            configure(path)
            self.assertAlmostEqual(opening_template(2,3,'window')['glazing'][:,2].min(),-.22)
            result=snapshot(directory)
            effective=json.loads(Path(result['effective_config']).read_text())
            self.assertEqual(effective['windows']['recess_m'],.22)
            self.assertEqual(len(result['config_sha256']),64)
            self.assertEqual(hashlib.sha256(Path(result['effective_config']).read_bytes()).hexdigest(),result['config_sha256'])

    def test_bad_rules_are_rejected_without_changing_active_rules(self):
        before=copy.deepcopy(RULES)
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'bad.json'
            for value in ({'windows':{'reces_m':1}}, {'roofs':{'mesh_resolution_m':0}},
                          {'doors':{'min_observed_fraction':1.1}}, {'source':{'preserve_capture':False}}):
                path.write_text(json.dumps(value))
                with self.assertRaises(ValueError):configure(path)
                self.assertEqual(RULES,before)


if __name__=='__main__':unittest.main()
