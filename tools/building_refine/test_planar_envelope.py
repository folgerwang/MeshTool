import copy,unittest
import numpy as np
from shapely.geometry import box,Polygon
import building,mtscene,raster
from planar_envelope import fit,assess,polygonal_boundary
from solid_city import shell,audit_shell
from stepped_shell import build
from install_building_repairs import planarity


class PlanarEnvelopeTest(unittest.TestCase):
    def group(self):
        return mtscene.Group([],shell(box(0,0,20,15),0,10,0),[mtscene.SceneObject('building',3)])

    def model(self):
        footprint=box(0,0,20,15);detail=box(4,4,7,7)
        return building.BuildingModel(0,footprint,[building.RoofPart(footprint.difference(detail),np.array([0.,0.,10.])),building.RoofPart(detail,np.array([.3,.2,9.]))])

    def test_minor_roof_detail_does_not_veto_flat_body(self):
        g=self.group();layers,report=fit(self.model(),raster.collect(g).pos)
        self.assertGreater(report['dominant_flat_roof_fraction'],.95);self.assertEqual(len(layers),1)
        p,_=build(layers,0);m=mtscene.Mesh(0xffffffff,0,np.zeros(3),p.astype(np.float32),None,None,[(4,np.arange(len(p),dtype=np.uint32))])
        self.assertFalse(any(audit_shell([m]).values()));self.assertEqual(report['wall_segments'],4)

    def test_missing_captured_wall_can_be_completed_from_roof(self):
        candidate=self.group();source=copy.deepcopy(candidate)
        m=source.meshes[0];tri=m.vertices[m.draw_calls[0][1]].reshape(-1,3,3)+m.translation
        keep=~(abs(tri[:,:,0])<1e-5).all(axis=1);m.draw_calls=[(4,m.draw_calls[0][1].reshape(-1,3)[keep].ravel())]
        result=assess(source,candidate,[(10.,box(0,0,20,15))]);self.assertTrue(result['accepted'])

    def test_unsupported_expansion_is_rejected(self):
        source=self.group();candidate=copy.deepcopy(source)
        for m in candidate.meshes:m.vertices[:,0]*=2
        self.assertFalse(assess(source,candidate,[(10.,box(-10,0,30,15))])['accepted'])

    def test_genuinely_sloping_main_roof_is_not_flattened(self):
        model=self.model()
        for p in model.parts:p.plane=np.array([.4,.3,10.])
        with self.assertRaisesRegex(ValueError,'not predominantly flat'):fit(model,raster.collect(self.group()).pos)

    def test_wall_validation_rejects_warped_vertices(self):
        vertices=np.array([[0,0,0],[0,2,0],[0,2,3]],dtype=np.float32)
        mesh=mtscene.Mesh(0xffffffff,0,np.zeros(3),vertices,None,None,[(4,np.array([0,1,2],dtype=np.uint32))])
        group=mtscene.Group([],[mesh],[])
        catalog=dict(faces=[dict(normal=[1,0,0],origin=[0,0,0])],semantic_meshes=[dict(part='wall',local_mesh_index=0)])
        self.assertEqual(planarity(group,catalog)['wall_triangles_checked'],1)
        mesh.vertices[2,0]=.1
        with self.assertRaisesRegex(ValueError,'Nonplanar wall'):planarity(group,catalog)

    def test_wall_validation_requires_wall_geometry(self):
        with self.assertRaisesRegex(ValueError,'No wall triangles'):
            planarity(mtscene.Group([],[],[]),dict(faces=[dict(normal=[1,0,0],origin=[0,0,0])],semantic_meshes=[]))

    def test_oblique_facades_remain_planar_without_square_footprint(self):
        footprint=Polygon([(0,0),(30,0),(6,20)])
        model=building.BuildingModel(0,footprint,[building.RoofPart(footprint,np.array([0.,0.,10.]))])
        source=mtscene.Group([],shell(footprint,0,10,0),[])
        layers,report=fit(model,raster.collect(source).pos,polygonal=True)
        self.assertEqual(report['boundary_mode'],'polygonal')
        self.assertLess(layers[0][1].symmetric_difference(footprint).area,.01)
        points,_=build(layers,0)
        mesh=mtscene.Mesh(0xffffffff,0,np.zeros(3),points.astype(np.float32),None,None,[(4,np.arange(len(points),dtype=np.uint32))])
        self.assertFalse(any(audit_shell([mesh]).values()))

    def test_polygonal_fit_preserves_supported_courtyard(self):
        footprint=box(0,0,30,30).difference(box(10,10,20,20))
        result=polygonal_boundary(footprint,.8)
        self.assertEqual(len(result.interiors),1)
        self.assertAlmostEqual(result.area,footprint.area)


if __name__=='__main__':unittest.main()
