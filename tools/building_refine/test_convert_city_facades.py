import copy
import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
import numpy as np
from shapely.geometry import Polygon
import mtscene
from solid_city import shell,atlas,audit_shell
from convert_city_facades import run,retry_fallbacks


class CityFacadeBatchTest(unittest.TestCase):
    def fixture(self,path):
        active=shell(Polygon([(0,0),(12,0),(12,10),(0,10)]),0,12,0)
        for mesh in active:mesh.model_variant=2
        originals=copy.deepcopy(active)
        for mesh in originals:mesh.model_variant=1;mesh.vertices[:,0]+=.25
        other=shell(Polygon([(20,0),(24,0),(24,4),(20,4)]),0,6,1)
        group=mtscene.Group([atlas((180,170,160)),atlas((130,130,130))],originals+active+other,
            [mtscene.SceneObject('building_367',3),mtscene.SceneObject('building_other',3)])
        mtscene.save(path,mtscene.Scene([mtscene.Batch(False,True,False,(0,0),[group])]))
        return group

    def test_all_buildings_convert_without_touching_archived_originals(self):
        with tempfile.TemporaryDirectory() as temp:
            source=Path(temp)/'input.mtscene';original=self.fixture(source)
            digest=hashlib.sha256(source.read_bytes()).hexdigest();out=Path(temp)/'converted'
            run(source,out)
            result=mtscene.load(out/'city.mtscene')[0].groups[0]
            archived=[m for m in result.meshes if m.model_variant==1]
            expected=[m for m in original.meshes if m.model_variant==1]
            self.assertEqual(len(archived),len(expected))
            for actual,before in zip(archived,expected):
                np.testing.assert_array_equal(actual.vertices,before.vertices)
                np.testing.assert_array_equal(actual.uvs,before.uvs)
                self.assertEqual(actual.tex_index,before.tex_index)
            manifest=json.loads((out/'manifest.json').read_text())
            self.assertEqual(manifest['total'],2)
            self.assertEqual(manifest['statuses'],{'converted':2})
            self.assertEqual(hashlib.sha256(source.read_bytes()).hexdigest(),digest)
            self.assertTrue(all(r['audit_before']==r['audit_after'] for r in manifest['buildings']))

    def test_failed_proposal_retains_topology_and_can_be_retried(self):
        with tempfile.TemporaryDirectory() as temp:
            source=Path(temp)/'input.mtscene';self.fixture(source);out=Path(temp)/'converted'
            with patch('convert_city_facades.refine_building',side_effect=ValueError('Rejected proposal')):
                run(source,out)
            manifest=json.loads((out/'manifest.json').read_text())
            self.assertEqual(manifest['statuses'],{'material_only_fallback':2})
            self.assertTrue(all(r['audit_before']==r['audit_after'] for r in manifest['buildings']))
            retry_fallbacks(out)
            manifest=json.loads((out/'manifest.json').read_text())
            self.assertEqual(manifest['statuses'],{'converted':2})
            self.assertTrue(manifest['retry_completed'])


if __name__=='__main__':unittest.main()
