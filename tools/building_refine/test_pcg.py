"""Selected/batch integration and geometry regression checks without ML downloads."""
import contextlib
import hashlib
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import numpy as np
from shapely.geometry import Polygon

import building
import mtscene
import pcg
import refine


def model(x=0):
    outline = Polygon([(x,0),(x+12,0),(x+12,10),(x,10)])
    return building.BuildingModel(0, outline, [building.RoofPart(outline,np.array([0.,0.,12.]))])


def mesh_from_model(m, oid):
    p = np.concatenate([f.tris.reshape(-1,3) for f in building.model_faces(m)])
    return mtscene.Mesh(0xffffffff,oid,np.zeros(3),p.astype(np.float32),None,None,
                       [(4,np.arange(len(p),dtype=np.uint32))])


def fixture():
    floor = np.array([[-30,-30,-.01],[100,-30,-.01],[100,50,-.01],[-30,-30,-.01],
                      [100,50,-.01],[-30,50,-.01]],np.float32)
    ground = mtscene.Mesh(0xffffffff,2,np.zeros(3),floor,None,None,[(4,np.arange(6,dtype=np.uint32))])
    g = mtscene.Group([], [mesh_from_model(model(),0),mesh_from_model(model(30),1),ground],
                      [mtscene.SceneObject('building_shared',3),mtscene.SceneObject('neighbor',3),
                       mtscene.SceneObject('ground',1)])
    other = mtscene.Group([], [mesh_from_model(model(60),0)], [mtscene.SceneObject('building_shared',3)])
    return [mtscene.Batch(False,True,True,(-122.19,47.61),[g,other])]


def fingerprint(m):
    h=hashlib.sha256()
    h.update(str((m.tex_index,m.object_id,m.material,m.model_variant)).encode())
    for a in (m.translation,m.vertices,m.uvs,m.colors):
        if a is not None: h.update(a.tobytes())
    for prim,idx in m.draw_calls: h.update(str(prim).encode()); h.update(idx.tobytes())
    return h.hexdigest()


class PCGTest(unittest.TestCase):
    def run_refine(self, batches, flags):
        with tempfile.TemporaryDirectory() as d:
            inp,out=Path(d)/'in.mtscene',Path(d)/'out.mtscene'
            mtscene.save(inp,batches)
            with patch('sys.argv',['refine',str(inp),str(out),'--pcg','--no-sam','--no-glass',*flags]),contextlib.redirect_stdout(io.StringIO()) as log:
                self.assertEqual(refine.main(),0)
            return mtscene.load(out),log.getvalue()

    def test_batch_refines_both_neighboring_buildings(self):
        result,log=self.run_refine(fixture(),[])
        for oid in (0,1):
            self.assertTrue(any(m.object_id==oid and m.material==pcg.MAT_FACADE
                                for m in result[0].groups[0].meshes),log)

    def test_reapply_replaces_details_and_failed_fit_retains_old_details(self):
        first,log=self.run_refine(fixture(),['--target','0:0:0'])
        def parts(b): return [m for m in b[0].groups[0].meshes if m.object_id==0 and m.material in (4,5,6)]
        self.assertTrue(parts(first),log)
        second,log=self.run_refine(first,['--target','0:0:0'])
        self.assertIn('PCG:',log,log)
        self.assertLessEqual(sum(len(m.vertices) for m in parts(second)),sum(len(m.vertices) for m in parts(first)))
        with patch('building.build_model',return_value=None):
            rejected,_=self.run_refine(first,['--target','0:0:0'])
        self.assertEqual([fingerprint(m) for m in parts(first)],[fingerprint(m) for m in parts(rejected)])

    def test_glass_retains_translucent_and_interior_materials_with_pcg(self):
        import raster
        g=fixture()[0].groups[0]
        src=raster.collect(g)
        class AllGlass:
            def p_glass(self,crops): return np.ones(len(crops))
        m=model()
        meshes,_,_,_,area=refine.model_meshes(m,building.model_faces(m),src,
            raster.decode_textures(g),.5,0,0,AllGlass())
        self.assertGreater(area,100)
        self.assertIn(mtscene.MAT_GLASS,[x.material for x in meshes])
        self.assertIn(mtscene.MAT_INTERIOR,[x.material for x in meshes])
        for x in meshes:
            if x.material==mtscene.MAT_CAPTURED: x.material=pcg.MAT_FACADE
        self.assertIn(mtscene.MAT_GLASS,[x.material for x in meshes])

    def test_details_are_finite_grouped_and_belong_to_target(self):
        m=model()
        details,count=pcg.detail_meshes(building.model_faces(m),7,m)
        self.assertGreater(count,30)
        self.assertLessEqual(len(details),3)
        self.assertTrue(all(x.object_id==7 and np.isfinite(x.vertices).all() for x in details))
        self.assertEqual(sum(len(x.vertices) for x in details),count*36)
        # Every detail lies outside the wall or above the roof, not inside
        # the opaque facade where offsets would be invisible/z-fighting.
        p=np.concatenate([x.vertices+x.translation for x in details])
        in_plan=(p[:,0]>.001)&(p[:,0]<11.999)&(p[:,1]>.001)&(p[:,1]<9.999)
        self.assertTrue(np.all(p[in_plan,2]>12.0))

    def test_roof_equipment_does_not_fill_courtyard(self):
        outer=Polygon([(0,0),(20,0),(20,20),(0,20)],holes=[[(4,4),(16,4),(16,16),(4,16)]])
        m=building.BuildingModel(0,outer,[building.RoofPart(outer,np.array([0.,0.,12.]))])
        details,_=pcg.detail_meshes(building.model_faces(m),0,m)
        roof=np.concatenate([x.vertices+x.translation for x in details if x.material==pcg.MAT_ROOF])
        in_hole=(roof[:,0]>4)&(roof[:,0]<16)&(roof[:,1]>4)&(roof[:,1]<16)
        self.assertFalse(in_hole.any())

    def test_scene_roundtrip_retains_finishes(self):
        batches=fixture()
        m=model()
        details,_=pcg.detail_meshes(building.model_faces(m),0,m)
        batches[0].groups[0].meshes+=details
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)/'roundtrip.mtscene'; mtscene.save(p,batches)
            loaded=mtscene.load(p)
            self.assertEqual([fingerprint(x) for x in batches[0].groups[0].meshes],
                             [fingerprint(x) for x in loaded[0].groups[0].meshes])

    def test_selected_only_changes_exact_target(self):
        batches=fixture()
        unchanged=[fingerprint(x) for x in batches[0].groups[0].meshes if x.object_id!=0]
        other=[fingerprint(x) for x in batches[0].groups[1].meshes]
        with tempfile.TemporaryDirectory() as d:
            inp,out=Path(d)/'in.mtscene',Path(d)/'out.mtscene'
            mtscene.save(inp,batches)
            with patch('sys.argv',['refine',str(inp),str(out),'--pcg','--target','0:0:0',
                                   '--no-sam','--no-glass']),contextlib.redirect_stdout(io.StringIO()) as log:
                self.assertEqual(refine.main(),0)
            loaded=mtscene.load(out)
            self.assertIn('PCG:',log.getvalue(),log.getvalue())
            g=loaded[0].groups[0]
            self.assertEqual(unchanged,[fingerprint(x) for x in g.meshes if x.object_id!=0])
            self.assertEqual(other,[fingerprint(x) for x in loaded[0].groups[1].meshes])
            self.assertTrue(any(x.material==pcg.MAT_FACADE for x in g.meshes if x.object_id==0))

    def test_invalid_selection_and_global_cleanup_fail_before_loading(self):
        for flags in (['--target','0:0:0','--cull'],['--target','0:0:0','--clean'],['--target','-1:0:0']):
            with patch('sys.argv',['refine','missing','out',*flags]),contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as e: refine.main()
                self.assertEqual(e.exception.code,2)


if __name__=='__main__': unittest.main()
