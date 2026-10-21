import copy
import json
import tempfile
import unittest
from pathlib import Path
import numpy as np
from shapely.geometry import box,mapping
import mtscene
from stepped_shell import build
from capture_tiles import capture_faces
from solid_city import atlas,audit_shell
from repair_stepped_building import geometry_digest,repair


class SteppedBuildingRepairTest(unittest.TestCase):
    def fixture(self,directory):
        layers=[(9,box(0,0,18,15)),(24,box(3,3,12,12))]
        points,_=build(layers,0);center=points.mean(0)
        original=mtscene.Mesh(0,0,center,(points-center).astype(np.float32),np.zeros((len(points),2),np.float32),None,[(4,np.arange(len(points),dtype=np.uint32))],0,1)
        for face,index in capture_faces(original):
            rel=original.vertices[index].astype(float)+center-face.origin
            original.uvs[index]=np.c_[rel@face.u/face.size[0],1-rel@face.v/face.size[1]]
        damaged=copy.deepcopy(original);damaged.model_variant=2;damaged.draw_calls=[(4,damaged.draw_calls[0][1][6:])]
        other=copy.deepcopy(original);other.object_id=1;other.model_variant=0;other.translation+=np.array([40,0,0])
        group=mtscene.Group([atlas((180,175,165))],[original,damaged,other],[mtscene.SceneObject('building_188',3),mtscene.SceneObject('neighbor',3)])
        source=directory/'input.mtscene';mtscene.save(source,mtscene.Scene([mtscene.Batch(False,True,False,(0,0),[group])]))
        profile=directory/'profile.json';profile.write_text(json.dumps(dict(building='building_188',ground_z=0,
            source_geometry_sha256=geometry_digest([original]),layers=[dict(height=z,geometry=mapping(g)) for z,g in layers])))
        return source,profile,original,other

    def test_repair_closes_target_and_preserves_neighbor_and_original(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);source,profile,original,other=self.fixture(root);before=source.read_bytes()
            repair(source,profile,root/'out')
            result=mtscene.load(root/'out/city.mtscene')[0].groups[0]
            active=[m for m in result.meshes if m.object_id==0 and m.model_variant==2]
            self.assertFalse(any(audit_shell(active).values()))
            retained=[m for m in result.meshes if m.object_id==0 and m.model_variant==1]
            self.assertEqual(geometry_digest(retained),geometry_digest([original]))
            neighbor=next(m for m in result.meshes if m.object_id==1)
            np.testing.assert_array_equal(neighbor.vertices,other.vertices)
            np.testing.assert_array_equal(neighbor.uvs,other.uvs)
            self.assertEqual(source.read_bytes(),before)
            report=json.loads((root/'out/repair_report.json').read_text())
            self.assertTrue(report['window_wall_cutouts'])

    def test_profile_for_different_capture_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);source,profile,_,_=self.fixture(root)
            data=json.loads(profile.read_text());data['source_geometry_sha256']='incorrect';profile.write_text(json.dumps(data))
            with self.assertRaisesRegex(ValueError,'does not match'):repair(source,profile,root/'out')


if __name__=='__main__':unittest.main()
