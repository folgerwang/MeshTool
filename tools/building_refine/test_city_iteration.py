import json,tempfile,unittest
from pathlib import Path
import numpy as np
import mtscene
import tree_instances,iterate_roads,iterate_building_shells
from test_trees import fixture


class CityIterationTest(unittest.TestCase):
    def data(self):
        h=w=180;y,x=np.mgrid[:h,:w]
        road=abs(x-90)<18
        return dict(height=np.zeros((h,w),np.float32),cover=np.ones((h,w),bool),source_class=np.where(road,2,1),
                    surface_class=np.where(road,2,1),captured_height=np.zeros((h,w)),inferred=np.zeros((h,w),bool),res=.5,min_x=0.,max_y=90.)

    def test_tree_prototypes_are_shared_gltf_meshes(self):
        lib=tree_instances.prototypes()
        instances=[dict(source_object='tree',prototype=0,position=[1.,2.,3.],scale=[5.,6.,8.]) for _ in range(4)]
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'trees.gltf';tree_instances.export_gltf(lib,instances,p);doc=json.loads(p.read_text())
            self.assertEqual(len(doc['meshes']),3);self.assertEqual([n['mesh'] for n in doc['nodes'][1:]],[0]*4)
            self.assertEqual(p.with_suffix('.bin').stat().st_size,doc['buffers'][0]['byteLength'])

    def test_tree_grounding_and_capture_removal(self):
        scene=fixture();g=scene[0].groups[0];data=self.data()
        g.objects.append(mtscene.SceneObject('car',4));m=g.meshes[0]
        import copy
        car=copy.deepcopy(m);car.object_id=4;g.meshes.append(car)
        with tempfile.TemporaryDirectory() as d:
            report=tree_instances.apply(g,data,d)
            self.assertEqual(report['removed_car_objects'],1)
            self.assertFalse(any(g.objects[m.object_id].cls==4 for m in g.meshes))
            self.assertTrue(report['instances'])
            self.assertTrue(all(i['position'][2]==0 for i in report['instances']))
            self.assertTrue(all(m.model_variant==2 for m in g.meshes if g.objects[m.object_id].cls==5))

    def test_tree_trunks_do_not_land_on_buildings(self):
        g=fixture()[0].groups[0];data=self.data();data['source_class'][:]=3
        fits,_=tree_instances.placements([g.meshes[0]],data)
        self.assertEqual(fits,[])

    def test_mask_never_covers_buildings_and_splines_stay_bounded(self):
        data=self.data();excluded=np.zeros_like(data['cover']);excluded[60:80,75:82]=True
        with tempfile.TemporaryDirectory() as d:
            road,report=iterate_roads.choose_mask(data,excluded,d)
        self.assertFalse((road&excluded).any());self.assertGreater(report['iterations'][0]['captured_road_recall'],.99)
        paths=iterate_roads.centerlines(road,data);self.assertTrue(paths)
        self.assertLessEqual(max(p['max_fit_shift_m'] for p in paths),.500001)

    def test_paint_preserves_shapes_and_excludes_occluded_car(self):
        rgb=np.full((100,100,3),85,np.uint8);rgb[20:25,20:60]=220;rgb[35:60,20:25]=220
        rgb[60:68,60:74]=240;valid=np.ones((100,100),bool);valid[60:68,60:74]=False
        masks=iterate_roads.paint_masks(rgb,np.ones_like(valid),valid,.25,165)
        self.assertTrue(masks['white'][22,40]);self.assertTrue(masks['white'][45,22]);self.assertFalse(masks['white'][64,65])

    def test_capture_comparison_rejects_shorter_building(self):
        import copy
        g=fixture()[0].groups[0];source=mtscene.Group(g.textures,[g.meshes[1]],[mtscene.SceneObject('building',3)])
        source.meshes[0]=copy.deepcopy(source.meshes[0]);source.meshes[0].object_id=0
        candidate=copy.deepcopy(source)
        same=iterate_building_shells.compare(source,candidate);self.assertTrue(same['accepted'])
        candidate.meshes[0].vertices[:,2]*=.5
        self.assertFalse(iterate_building_shells.compare(source,candidate)['accepted'])

    def test_paving_replaces_photo_without_changing_geometry(self):
        from compose_iteration import clean_paving,compact_textures
        g=fixture()[0].groups[0];before=[m.vertices.copy() for m in g.meshes]
        data=self.data();color=np.full((*data['height'].shape,3),145,np.uint8)
        report=clean_paving(g,data,color);self.assertEqual(report['meshes'],1)
        for m,p in zip(g.meshes,before):np.testing.assert_array_equal(m.vertices,p)
        textures=[g.textures[m.tex_index] if m.tex_index<len(g.textures) else None for m in g.meshes]
        compact_textures(g)
        for m,t in zip(g.meshes,textures):
            if t is not None:self.assertEqual(g.textures[m.tex_index],t)

    def test_reviewed_vehicle_requires_matching_geometry_before_removal(self):
        from compose_iteration import remove_reviewed_vehicles,fingerprint
        from repair_stepped_building import geometry_digest
        g=fixture()[0].groups[0];own=[m for m in g.meshes if m.object_id==1]
        profile=dict(objects=[dict(name='building_000',expected_geometry_sha256='invalid')]);before=fingerprint(g.meshes)
        with self.assertRaises(ValueError):remove_reviewed_vehicles(g,profile)
        self.assertEqual(before,fingerprint(g.meshes));self.assertEqual(g.objects[1].cls,3)
        profile['objects'][0]['expected_geometry_sha256']=geometry_digest(own)
        self.assertEqual(remove_reviewed_vehicles(g,profile),['building_000'])
        self.assertEqual(g.objects[1].cls,4);self.assertFalse(any(m.object_id==1 for m in g.meshes))


if __name__=='__main__':unittest.main()
