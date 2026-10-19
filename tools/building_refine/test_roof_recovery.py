"""Roof-supported completion must close scan gaps and preserve architecture."""
import unittest
import numpy as np
from shapely.geometry import Polygon
import mtscene
import regular_facade
import facade
from test_pcg import mesh_from_model, model, fingerprint


def mesh(triangles,texture=0xffffffff):
    p=np.asarray(triangles,dtype=np.float32).reshape(-1,3)
    return mtscene.Mesh(texture,0,np.zeros(3),p,np.zeros((len(p),2),np.float32),None,
                        [(4,np.arange(len(p),dtype=np.uint32))])


def fixture(courtyard=False):
    outline=[(0,0),(24,0),(24,20),(0,20)]
    walls=[]
    for a,b in zip(outline,outline[1:]+outline[:1]):
        for lo,hi in [(0,2),(18,24)]:
            p,q,r,s=[[*a,lo],[*b,lo],[*b,hi],[*a,hi]]
            walls.extend([[p,q,r],[p,r,s]])
    roof=mesh([[[0,0,24],[24,0,24],[24,20,24]],[[0,0,24],[24,20,24],[0,20,24]]],0)
    source=[mesh(walls),roof]
    if courtyard:
        inner=[(6,5),(18,5),(18,15),(6,15)]
        p=[]
        for a,b in zip(inner,inner[1:]+inner[:1]):
            p.extend([[[*a,0],[*b,0],[*b,24]],[[*a,0],[*b,24],[*a,24]]])
        source.append(mesh(p))
    return source


class RoofRecoveryTest(unittest.TestCase):
    def test_missing_wall_bands_become_four_continuous_planes(self):
        source=fixture();before=[fingerprint(m) for m in source]
        proxy,planes,stats=regular_facade.rebuild(source)
        self.assertEqual(stats['envelope_method'],'roof_supported')
        self.assertEqual(len(planes),4)
        for p in planes:
            xyz=np.asarray(p['tri']);u=np.cross([0.,0.,1.],p['n'])
            from shapely.ops import unary_union
            region=unary_union([Polygon(np.column_stack([t@u,t[:,2]])) for t in xyz])
            self.assertAlmostEqual(region.area,region.envelope.area,places=4)
            self.assertAlmostEqual(xyz[:,:,2].min(),0.)
            self.assertAlmostEqual(xyz[:,:,2].max(),24.)
        self.assertEqual(before,[fingerprint(m) for m in source])

    def test_textured_roof_survives_with_identical_uvs_and_triangles(self):
        source=fixture();roof=source[1]
        tex=mtscene.Texture(0x8058,0x1908,0x1401,[(4,4,bytes([160,150,140,255])*16)])
        result,_,_=facade.build(source,0,1,[tex])
        retained=[m for m in result if m.material==0 and m.tex_index==0]
        self.assertEqual(len(retained),1)
        np.testing.assert_array_equal(retained[0].vertices,roof.vertices)
        np.testing.assert_array_equal(retained[0].uvs,roof.uvs)
        np.testing.assert_array_equal(retained[0].draw_calls[0][1],roof.draw_calls[0][1])

    def test_courtyard_walls_prevent_outer_box_completion(self):
        _,_,stats=regular_facade.rebuild(fixture(courtyard=True))
        self.assertFalse(stats['closed_envelope'])

    def test_existing_complete_shell_does_not_get_an_extra_deck(self):
        _,_,stats=regular_facade.rebuild([mesh_from_model(model(),0)])
        self.assertFalse(stats['closed_envelope'])

    def test_separated_wings_without_roof_support_remain_separate(self):
        source=fixture();source.pop(1)
        _,_,stats=regular_facade.rebuild(source)
        self.assertFalse(stats['closed_envelope'])


if __name__=='__main__':unittest.main()
