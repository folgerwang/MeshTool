"""Replacement-facade geometry, persistence, and reapplication checks."""
import unittest
from unittest.mock import patch
import numpy as np
import facade
import mtscene
import test_pcg

class FacadeTest(unittest.TestCase):
    def test_replaces_wall_surfaces_with_recessed_window_geometry(self):
        source=test_pcg.mesh_from_model(test_pcg.model(),0)
        before=test_pcg.fingerprint(source)
        meshes,textures,stats=facade.build([source],0,0)
        self.assertGreater(stats['windows'],20)
        self.assertAlmostEqual(stats['source_area'],stats['covered_area'],places=5)
        self.assertEqual(len(textures),3)
        self.assertEqual(before,test_pcg.fingerprint(source))
        self.assertTrue({1,2,3,4}.issubset({m.material for m in meshes}))
        for m in meshes:
            self.assertEqual(m.model_variant,2)
            self.assertTrue(np.isfinite(m.vertices).all())
            if m.uvs is not None:self.assertTrue(np.isfinite(m.uvs).all())
            if m.material==0:
                for _,idx in m.draw_calls:
                    t=m.vertices[idx.reshape(-1,3)]
                    n=np.cross(t[:,1]-t[:,0],t[:,2]-t[:,0])
                    self.assertTrue((np.abs(n[:,2])>.2*np.linalg.norm(n,axis=1)).all())
            if m.material==1:
                p=m.vertices+m.translation
                distance=np.stack([np.abs(p[:,0]),np.abs(p[:,0]-12),np.abs(p[:,1]),np.abs(p[:,1]-10)])
                self.assertTrue(np.isclose(distance,.14,atol=1e-5).any(axis=0).all())

    def test_exact_target_original_and_neighbors_survive_reapply(self):
        batches=test_pcg.fixture();g=batches[0].groups[0]
        original=test_pcg.fingerprint(g.meshes[0]);neighbor=test_pcg.fingerprint(g.meshes[1])
        self.assertEqual(facade.apply(batches,(0,0,0)),(1,1))
        archive=next(m for m in g.meshes if m.model_variant==1)
        archive.model_variant=0;self.assertEqual(original,test_pcg.fingerprint(archive));archive.model_variant=1
        self.assertEqual(neighbor,test_pcg.fingerprint(next(m for m in g.meshes if m.object_id==1)))
        first=[test_pcg.fingerprint(m) for m in g.meshes];textures=len(g.textures)
        facade.apply(batches,(0,0,0))
        self.assertEqual(textures,len(g.textures))
        self.assertEqual(first,[test_pcg.fingerprint(m) for m in g.meshes])

    def test_unusable_or_over_budget_keeps_existing_scene(self):
        batches=test_pcg.fixture();g=batches[0].groups[0]
        before=[test_pcg.fingerprint(m) for m in g.meshes]
        with patch('facade.build',side_effect=ValueError('budget')):
            self.assertEqual(facade.apply(batches,(0,0,0)),(0,1))
        self.assertEqual(before,[test_pcg.fingerprint(m) for m in g.meshes])
        self.assertFalse(g.textures)

    def test_boundary_window_shrinks_without_crossing_diagonal_edge(self):
        from shapely.geometry import Polygon,box
        tile=Polygon([(0,0),(1.6,0),(1.6,3.4),(.8,3.4)])
        requested=box(.1,.65,1.5,3.26)
        self.assertFalse(tile.covers(requested))
        fitted=facade.fit_window(tile,requested)
        self.assertGreater(fitted.area,.4)
        self.assertTrue(tile.buffer(1e-6).covers(fitted))
        self.assertAlmostEqual(fitted.area,facade.box(*fitted.bounds).area)
        self.assertTrue(fitted.equals(facade.fit_window(tile,requested)))

    def test_boundary_window_does_not_span_a_hole(self):
        from shapely.geometry import box
        tile=box(0,0,1.6,3.4).difference(box(.65,.4,.95,3.4))
        fitted=facade.fit_window(tile,box(.1,.65,1.5,3.26))
        self.assertFalse(fitted.is_empty)
        self.assertTrue(tile.buffer(1e-6).covers(fitted))

    def test_wall_finish_changes_panel_material_but_retains_glass(self):
        source=test_pcg.mesh_from_model(test_pcg.model(),0)
        concrete,ct,cs=facade.build([source],0,0,wall_finish="painted-concrete")
        metal,mt,ms=facade.build([source],0,0,wall_finish="metal")
        self.assertEqual({m.material for m in concrete if m.tex_index==0},{3})
        self.assertEqual({m.material for m in metal if m.tex_index==0},{4})
        self.assertTrue(any(m.material==1 for m in metal))
        self.assertEqual(cs['windows'],ms['windows'])
        self.assertNotEqual(ct[0].mips,mt[0].mips)

    def test_wall_gap_is_not_filled_with_windows(self):
        def panel(x0,x1):return np.array([[[x0,0,0],[x1,0,0],[x1,0,8]],[[x0,0,0],[x1,0,8],[x0,0,8]]],float)
        verts=np.concatenate([panel(0,5),panel(8,13)]).reshape(-1,3)
        source=mtscene.Mesh(0xffffffff,0,np.zeros(3),verts,None,None,[(4,np.arange(len(verts),dtype=np.uint32))])
        meshes,_,stats=facade.build([source],0,0)
        self.assertGreater(stats['windows'],0)
        for m in meshes:
            p=m.vertices+m.translation
            self.assertFalse(((p[:,0]>5.001)&(p[:,0]<7.999)).any())

if __name__=='__main__':unittest.main()
