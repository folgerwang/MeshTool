import json
import tempfile
import unittest
from pathlib import Path
import numpy as np
from shapely.geometry import Polygon,box
import mtscene
from stepped_shell import build
from solid_city import audit_shell,atlas
from capture_tiles import capture_faces,refine_building


def mesh_for(points):
    center=points.mean(0)
    return mtscene.Mesh(0,0,center,(points-center).astype(np.float32),np.zeros((len(points),2),np.float32),None,[(4,np.arange(len(points),dtype=np.uint32))])


class SteppedShellTest(unittest.TestCase):
    def test_tower_and_podium_share_closed_boundaries(self):
        points,_=build([(8,box(0,0,24,18)),(32,Polygon([(3,3),(9,3),(12,6),(12,12),(9,15),(3,15),(0,12),(0,6)]))],0)
        self.assertFalse(any(audit_shell([mesh_for(points)]).values()))

    def test_courtyard_remains_open_in_plan(self):
        podium=Polygon([(0,0),(24,0),(24,24),(0,24)],holes=[[(9,9),(9,15),(15,15),(15,9)]])
        points,roles=build([(8,podium),(20,box(1,1,7,7))],0)
        self.assertFalse(any(audit_shell([mesh_for(points)]).values()))
        for triangle,role in zip(points.reshape(-1,3,3),roles):
            if role in ('roof','base'):
                self.assertLess(Polygon(triangle[:,:2]).intersection(box(9.01,9.01,14.99,14.99)).area,1e-8)

    def test_unsupported_upper_footprint_is_rejected(self):
        with self.assertRaises(ValueError):build([(8,box(0,0,10,10)),(20,box(8,8,15,15))],0)

    def test_windows_cut_wall_and_keep_stepped_envelope_closed(self):
        points,_=build([(9,box(0,0,18,15)),(24,box(3,3,12,12))],0)
        mesh=mesh_for(points)
        for face,indices in capture_faces(mesh):
            rel=mesh.vertices[indices].astype(float)+mesh.translation-face.origin
            mesh.uvs[indices]=np.c_[rel@face.u/face.size[0],1-rel@face.v/face.size[1]]
        with tempfile.TemporaryDirectory() as temp:
            result,report=refine_building([mesh],mtscene.SceneObject('tower_podium',3),[atlas((180,175,165))],Path(temp),clean=True)
            catalog=json.loads((Path(temp)/'tiles.json').read_text())
            self.assertGreater(report['counts'].get('window',0),0)
            self.assertFalse(any(audit_shell(result).values()),audit_shell(result))
            walls=[result[r['local_mesh_index']] for r in catalog['semantic_meshes'] if r['part']=='wall']
            triangles=np.concatenate([m.vertices[i].astype(float)+m.translation for m in walls for prim,i in m.draw_calls if prim==4]).reshape(-1,3,3)
            faces={f['face_id']:f for f in catalog['faces']}
            for instance in catalog['instances']:
                if instance.get('kind')!='window':continue
                face=faces[instance['face_id']];rel=triangles-np.array(face['origin'])
                on_plane=np.max(np.abs(rel@np.array(face['normal'])),axis=1)<1e-4
                xy=np.stack([rel[on_plane]@np.array(face['u']),rel[on_plane]@np.array(face['v'])],axis=-1)
                opening=box(*instance['bounds']).buffer(-.005)
                self.assertLess(sum(Polygon(t).intersection(opening).area for t in xy),1e-5)


if __name__=='__main__':unittest.main()
