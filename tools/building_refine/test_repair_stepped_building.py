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
from repair_stepped_building import geometry_digest,repair,reviewed_openings


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

    def test_reviewed_windows_keep_bounds_members_and_closed_shell(self):
        with tempfile.TemporaryDirectory() as temp:
            root=Path(temp);source,profile,_,_=self.fixture(root)
            repair(source,profile,root/'reference')
            catalog=json.loads((root/'reference/building_188/tiles.json').read_text())
            face=next(f for f in catalog['faces'] if min(f['size'])>5)
            data=json.loads(profile.read_text())
            config={k:face[k] for k in ('origin','u','v','normal','size')}
            config['openings']=[dict(bounds=[1.,1.,4.,4.5],measured_members=[dict(axis='x',position=.4,width_m=.1,support=1.)]),
                dict(kind='door_candidate',bounds=[6.,.04,8.,3.],measured_members=[dict(axis='x',position=.5,width_m=.1,support=1.)])]
            data['facades']={str(face['face_id']):config};data['facade_details']=True
            data['window_style']=dict(frame_width_m=.18,frame_rgb=[190,185,175],mullion_rgb=[190,185,175],glazing_rgb=[30,50,70],door_leaf_rgb=[85,60,40])
            profile.write_text(json.dumps(data))
            repair(source,profile,root/'reviewed')
            catalog=json.loads((root/'reviewed/building_188/tiles.json').read_text())
            windows=[i for i in catalog['instances'] if i.get('kind')=='window']
            self.assertEqual(len(windows),1)
            np.testing.assert_allclose(windows[0]['bounds'],[1.,1.,4.,4.5])
            prototype=next(p for p in catalog['prototypes'] if p['id']==windows[0]['prototype'])
            self.assertIn('mullion',prototype['geometry'])
            self.assertEqual(catalog['window_style'],data['window_style'])
            frame=np.asarray(prototype['geometry']['frame'])
            self.assertTrue(np.any(np.isclose(frame[:,0],.18/3,atol=1e-6)))
            from capture_tiles import decode
            asset=mtscene.load(root/'reviewed/building_188.mtscene')[0].groups[0]
            for part in ('frame','mullion','glazing','door_leaf'):
                meshes=[asset.meshes[r['local_mesh_index']] for r in catalog['semantic_meshes'] if r['part']==part]
                self.assertTrue(meshes)
                for mesh in meshes:
                    rgb=decode(asset.textures[mesh.tex_index]).mean((0,1))
                    np.testing.assert_allclose(rgb,data['window_style'][part+'_rgb'],atol=8)
            report=json.loads((root/'reviewed/repair_report.json').read_text())
            self.assertFalse(any(report['audit_after'].values()))
            self.assertGreater(report['facade_detailing']['counts']['window_sill'],0)
            doors=[i for i in catalog['instances'] if i.get('kind')=='door_candidate']
            self.assertEqual(len(doors),1)
            self.assertEqual(doors[0]['semantic_status'],'reviewed_ground_level_door')
            self.assertEqual(report['reviewed_window_validation']['filled_openings_checked'],2)
            from validate_city_iteration import filled_openings
            for role in ('glazing','door_leaf'):
                broken=copy.deepcopy(asset)
                index=next(r['local_mesh_index'] for r in catalog['semantic_meshes'] if r['part']==role)
                # Delete one actual infill triangle while leaving its catalog
                # prototype intact: a presence-only check must not accept this.
                mesh=broken.meshes[index];prim,ix=mesh.draw_calls[0];mesh.draw_calls=[(prim,ix[3:])]
                with self.assertRaisesRegex(ValueError,'Unfilled opening'):filled_openings(broken,catalog)

    def test_reviewed_frame_mismatch_and_overlap_are_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            _,_,mesh,_=self.fixture(Path(temp))
            face=next(f for f,_ in capture_faces(mesh) if f.kind=='wall' and min(f.size)>5)
            config={k:np.asarray(getattr(face,k)).tolist() for k in ('origin','u','v','normal','size')}
            config['openings']=[dict(bounds=[1,1,4,4]),dict(bounds=[2,2,5,5])]
            with self.assertRaisesRegex(ValueError,'overlap'):reviewed_openings([mesh],{'0':config})
            config['origin'][0]+=1
            with self.assertRaisesRegex(ValueError,'frame changed'):reviewed_openings([mesh],{'0':config})

    def test_reviewed_door_requires_ground_support(self):
        with tempfile.TemporaryDirectory() as temp:
            _,_,mesh,_=self.fixture(Path(temp))
            face=next(f for f,_ in capture_faces(mesh) if f.kind=='wall' and min(f.size)>5)
            config={k:np.asarray(getattr(face,k)).tolist() for k in ('origin','u','v','normal','size')}
            config['openings']=[dict(kind='door_candidate',bounds=[1.,2.,3.,5.])]
            with self.assertRaisesRegex(ValueError,'building base'):reviewed_openings([mesh],{'0':config},0.)


if __name__=='__main__':unittest.main()
