"""Replacement-facade geometry, persistence, and reapplication checks."""
import unittest
from unittest.mock import patch
import numpy as np
import facade
import mtscene
import test_pcg

class FacadeTest(unittest.TestCase):
    def test_surface_projection_keeps_overlap_order_and_boundary_fallback(self):
        xy=np.array([[[0.,0.],[1.,0.],[0.,1.]],[[0.,0.],[1.,0.],[0.,1.]]])
        z=np.array([[0.,1.,2.],[10.,11.,12.]])
        a=xy[:,1]-xy[:,0];b=xy[:,2]-xy[:,0]
        points=np.array([[.2,.3],[1.,0.],[2.,0.]])
        np.testing.assert_allclose(facade._surface_heights(points,xy,z,a,b,np.ones(2)),[.8,1.,1.])

    def test_multiple_facade_palettes_reuse_atlases_on_reapply(self):
        batches=test_pcg.fixture();g=batches[0].groups[0]
        build=facade.build
        palettes=[dict(normal=n,wall_color=c,glass_color=[35,45,55]) for n,c in
                  [([1,0,0],[150,160,170]),([-1,0,0],[120,130,140]),([0,1,0],[180,170,160])]]
        def colored(*args,**kwargs):
            if len(args)>5:return build(*args,**kwargs)
            parameters={**kwargs.pop('parameters',{}),'face_colors':palettes,'align_grid':True}
            return build(*args,**kwargs,parameters=parameters)
        with patch('facade.build',side_effect=colored):
            facade.apply(batches,(0,0,0));count=len(g.textures)
            self.assertGreater(count,3)
            first=[test_pcg.fingerprint(m) for m in g.meshes]
            facade.apply(batches,(0,0,0))
            self.assertEqual(count,len(g.textures))
            self.assertEqual(first,[test_pcg.fingerprint(m) for m in g.meshes])
            self.assertTrue(all(m.tex_index==0xffffffff or m.tex_index<count for m in g.meshes))

    def test_replaces_wall_surfaces_with_recessed_window_geometry(self):
        source=test_pcg.mesh_from_model(test_pcg.model(),0)
        before=test_pcg.fingerprint(source)
        meshes,textures,stats=facade.build([source],0,0)
        self.assertGreater(stats['windows'],20)
        self.assertAlmostEqual(stats['source_area'],stats['covered_area'],places=5)
        self.assertEqual(len(textures),facade.TEXTURE_COUNT)
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
                # Window panes sit 14 cm behind the wall; entrance door leaves 40 cm.
                self.assertTrue((np.isclose(distance,.14,atol=1e-5)|np.isclose(distance,.40,atol=1e-5)).any(axis=0).all())

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

    def test_shared_diagonal_is_stitched_but_real_corner_is_not(self):
        def group(tri):
            tri=np.array(tri,float)
            n=np.cross(tri[1]-tri[0],tri[2]-tri[0]);n/=np.linalg.norm(n)
            return {'n':n,'anchor':tri[0],'tri':[tri]}
        a=group([[0,0,0],[6,0,0],[6,.1,10]])
        b=group([[0,0,0],[6,.1,10],[0,0,10]])
        merged,count=facade.stitch_internal_seams([a,b])
        self.assertEqual(count,1)
        self.assertEqual(len(merged),1)
        # Same vertical edge at a real building corner must stay separate.
        c=group([[0,0,0],[0,0,10],[0,6,10]])
        self.assertEqual(facade.stitch_internal_seams([b,c])[1],0)
        # Open, unconnected boundary must not be filled.
        d=group([[10,0,0],[16,0,0],[16,0,10]])
        self.assertEqual(facade.stitch_internal_seams([a,d])[1],0)

    def test_warped_capture_produces_planar_glass_panes(self):
        vertices=np.array([[0,0,0],[10,0,0],[10,.3,16],
                           [0,0,0],[10,.3,16],[0,0,16]],np.float32)
        source=mtscene.Mesh(0xffffffff,0,np.zeros(3),vertices,None,None,[(4,np.arange(6,dtype=np.uint32))])
        meshes,_,stats=facade.build([source],0,0)
        self.assertGreater(stats['windows'],0)
        panes=np.concatenate([m.vertices+m.translation for m in meshes if m.material==1]).reshape(-1,6,3)
        for pane in panes:
            singular=np.linalg.svd(pane-pane.mean(0),compute_uv=False)
            self.assertLess(singular[-1],1e-5)

    def test_wall_gap_is_not_filled_with_windows(self):
        def panel(x0,x1):return np.array([[[x0,0,0],[x1,0,0],[x1,0,8]],[[x0,0,0],[x1,0,8],[x0,0,8]]],float)
        verts=np.concatenate([panel(0,5),panel(8,13)]).reshape(-1,3)
        source=mtscene.Mesh(0xffffffff,0,np.zeros(3),verts,None,None,[(4,np.arange(len(verts),dtype=np.uint32))])
        meshes,_,stats=facade.build([source],0,0)
        self.assertGreater(stats['windows'],0)
        for m in meshes:
            p=m.vertices+m.translation
            self.assertFalse(((p[:,0]>5.001)&(p[:,0]<7.999)).any())

    def test_ground_floor_parapet_and_roof_surfacing(self):
        source=test_pcg.mesh_from_model(test_pcg.model(),0)
        meshes,textures,stats=facade.build([source],0,10)
        modules=stats['modules']
        self.assertGreaterEqual(modules['storefront'],20)
        self.assertEqual(modules['entrance'],1)
        self.assertTrue(stats['entrance'])
        self.assertGreater(modules['parapets'],0)
        self.assertEqual(stats['roof_triangles'],2)
        roof=[m for m in meshes if m.material==5]
        self.assertEqual(len(roof),1)
        self.assertEqual(roof[0].tex_index,13)
        self.assertTrue(np.allclose((roof[0].vertices+roof[0].translation)[:,2],12))
        # No captured-material mesh is left: the roof was re-surfaced.
        self.assertFalse([m for m in meshes if m.material==0])
        # The canopy projects beyond the wall plane; the door is recessed deeper than panes.
        metal=np.concatenate([m.vertices+m.translation for m in meshes if m.material==4])
        self.assertTrue((metal[:,1]<-1.2).any() or (metal[:,1]>11.2).any() or (metal[:,0]<-1.2).any() or (metal[:,0]>13.2).any())
        panes=np.concatenate([m.vertices+m.translation for m in meshes if m.material==1])
        inset=np.stack([np.abs(panes[:,0]),np.abs(panes[:,0]-12),np.abs(panes[:,1]),np.abs(panes[:,1]-10)]).min(0)
        self.assertTrue(np.isclose(inset,.40,atol=1e-5).any())

    def test_low_fragments_stay_captured_and_textures_reused(self):
        verts=np.array([[0,0,0],[10,0,0],[10,0,16],[0,0,0],[10,0,16],[0,0,16],
                        [0,0,.5],[3,0,.5],[3,2,.5],      # ground-level non-wall fragment
                        [0,0,16],[10,0,16],[10,4,16]],np.float32)   # roof cap
        source=mtscene.Mesh(0xffffffff,0,np.zeros(3),verts,None,None,[(4,np.arange(12,dtype=np.uint32))])
        meshes,textures,stats=facade.build([source],0,0)
        self.assertEqual(stats['roof_triangles'],1)
        captured=[m for m in meshes if m.material==0]
        self.assertEqual(len(captured),1)
        self.assertEqual([len(idx) for _,idx in captured[0].draw_calls],[3])
        self.assertEqual(len(textures),facade.TEXTURE_COUNT)
        batches=test_pcg.fixture();g=batches[0].groups[0]
        facade.apply(batches,(0,0,0));facade.apply(batches,(0,0,1))
        self.assertEqual(len(g.textures),facade.TEXTURE_COUNT)

    def test_integer_bays_and_corner_posts_remove_sliver_windows(self):
        # A 12.3 m wall: eight bays of 1.54 m rather than seven full bays and a sliver.
        verts=np.array([[0,0,0],[12.3,0,0],[12.3,0,10],[0,0,0],[12.3,0,10],[0,0,10]],np.float32)
        source=mtscene.Mesh(0xffffffff,0,np.zeros(3),verts,None,None,[(4,np.arange(6,dtype=np.uint32))])
        meshes,_,stats=facade.build([source],0,0)
        panes=np.concatenate([m.vertices+m.translation for m in meshes if m.material==1])
        widths=[]
        for lo in np.arange(0,12.3,12.3/8):
            inside=panes[(panes[:,0]>lo+.01)&(panes[:,0]<lo+12.3/8-.01)&(panes[:,2]>4)&(panes[:,2]<6)]
            widths.append(np.ptp(inside[:,0]) if len(inside) else 0)
        self.assertTrue(all(w>.5 for w in widths),widths)
        self.assertAlmostEqual(min(widths[1:-1]),max(widths[1:-1]),places=3)
        # Corner posts: the end bays' panes stop further from the edge.
        self.assertGreater(panes[:,0].min(),.40);self.assertLess(panes[:,0].max(),12.3-.40)
        self.assertLess(widths[0],min(widths[1:-1])-.1)

    def test_patches_face_outward_by_ray_parity_whatever_the_winding(self):
        def quad(p0,p1,p2,p3):return np.array([p0,p1,p2,p0,p2,p3],float)
        # A 20 m deep building whose x=0 wall has a 2 m notch cut 5 m into it.
        a1=quad([0,0,0],[0,0,10],[0,4,10],[0,4,0]);a2=quad([0,6,0],[0,6,10],[0,10,10],[0,10,0])
        b=quad([20,0,0],[20,10,0],[20,10,10],[20,0,10])
        e1=quad([0,0,0],[20,0,0],[20,0,10],[0,0,10]);e2=quad([0,10,0],[0,10,10],[20,10,10],[20,10,0])
        s1=quad([0,4,0],[5,4,0],[5,4,10],[0,4,10]);s2=quad([0,6,0],[0,6,10],[5,6,10],[5,6,0])
        back_ok=quad([5,4,0],[5,4,10],[5,6,10],[5,6,0])          # faces -x into the notch
        back_bad=quad([5,4,0],[5,6,0],[5,6,10],[5,4,10])         # wound the wrong way
        # Each duplicated once, as captures often are; a wrongly wound main wall too.
        for back,a_first in ((back_ok,a1),(back_bad,a1[[0,2,1,3,5,4]])):
            verts=np.concatenate([a_first,a2,b,e1,e2,s1,s2,back,back,b]).astype(np.float32)
            source=mtscene.Mesh(0xffffffff,0,np.zeros(3),verts,None,None,[(4,np.arange(len(verts),dtype=np.uint32))])
            meshes,_,stats=facade.build([source],0,0)
            backing=np.concatenate([m.vertices+m.translation for m in meshes if m.material==2])
            # Backing cells sit 55 cm behind each wall, on the building side.
            on=lambda a,v:np.abs(a-v)<.01
            notch=backing[(on(backing[:,1],4)|on(backing[:,1],6))&(np.abs(backing[:,0]-5)<1)]
            self.assertTrue(len(notch));self.assertTrue(np.allclose(notch[:,0],5.55,atol=1e-4))
            front=backing[(np.abs(backing[:,0])<1)&(backing[:,1]>1)&(backing[:,1]<3.4)]
            self.assertTrue(len(front));self.assertTrue(np.allclose(front[:,0],.55,atol=1e-4))
            far=backing[(np.abs(backing[:,0]-20)<1)&(backing[:,1]>1)&(backing[:,1]<9)]
            self.assertTrue(len(far));self.assertTrue(np.allclose(far[:,0],19.45,atol=1e-4))
        self.assertGreaterEqual(stats['flipped_patches'],1)

if __name__=='__main__':unittest.main()
