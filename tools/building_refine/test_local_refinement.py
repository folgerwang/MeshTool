"""Selected refinement must scale with the local area, not city extents."""
import unittest
import contextlib
import io
from pathlib import Path
import tempfile
from unittest.mock import patch
import numpy as np
import mtscene
import raster
import refine
import test_pcg


class LocalRefinementTest(unittest.TestCase):
    def test_selected_operation_decodes_only_referenced_textures(self):
        batches=test_pcg.fixture()
        group=batches[0].groups[0]
        group.textures=[mtscene.Texture() for _ in range(100)]
        group.meshes[0].tex_index=99
        group.meshes[1].tex_index=98
        sizes=[]
        decode=raster.decode_textures
        def record(g,**kw):
            sizes.append(len(g.textures))
            return decode(g,**kw)
        with tempfile.TemporaryDirectory() as d:
            source,target=Path(d)/'input.mtscene',Path(d)/'output.mtscene'
            mtscene.save(source,batches)
            with patch('sys.argv',['refine',str(source),str(target),'--target','0:0:0','--no-sam','--no-glass']),patch('raster.decode_textures',side_effect=record),contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(refine.main(),0)
            self.assertEqual(sizes,[2])
            output=mtscene.load(target)
            self.assertEqual(output[0].groups[0].meshes[1].tex_index,98)

    def test_subset_keeps_original_triangle_indices_for_replacement(self):
        p=np.array([[10000,10000,0],[10001,10000,0],[10000,10001,0],
                    [0,0,2],[1,0,2],[0,1,2]],np.float32)
        m=mtscene.Mesh(0xffffffff,0,np.zeros(3),p,None,None,[(4,np.arange(6,dtype=np.uint32))])
        g=mtscene.Group([],[m],[mtscene.SceneObject('building',3)])
        tri=raster.collect(g,bounds=np.array([-2,-2,2,2]))
        np.testing.assert_array_equal(tri.local,[1])
        kept=refine.drop_triangles(g,tri,np.array([True]))
        np.testing.assert_array_equal(kept[0].draw_calls[0][1],[0,1,2])

    def test_giant_ground_triangle_cannot_expand_selected_raster(self):
        p=np.array([[-100000,-100000,0],[100000,-100000,0],[0,100000,0]],np.float32)
        m=mtscene.Mesh(0xffffffff,0,np.zeros(3),p,None,None,[(4,np.arange(3,dtype=np.uint32))])
        g=mtscene.Group([],[m],[mtscene.SceneObject('ground',1)])
        bounds=np.array([-20.,-20.,20.,20.])
        tri=raster.collect(g,bounds=bounds)
        updates=[]
        top=raster.top_down(tri,raster.decode_textures(g),.25,bounds=bounds,
                            progress=lambda *args:updates.append(args))
        self.assertEqual(top.height.shape,(160,160))
        self.assertTrue(np.isfinite(top.height).all())
        self.assertEqual(updates[-1],(1,1,160,160))

    def test_local_raster_matches_global_crop_including_depth_ties(self):
        p=np.array([[0,0,4],[10,0,4],[0,10,4]],np.float32)
        meshes=[mtscene.Mesh(0xffffffff,i,np.zeros(3),p.copy(),None,None,
                            [(4,np.arange(3,dtype=np.uint32))]) for i in range(2)]
        g=mtscene.Group([],meshes,[mtscene.SceneObject('first',3),mtscene.SceneObject('second',3)])
        tri=raster.collect(g); textures=raster.decode_textures(g)
        whole=raster.top_down(tri,textures,1.0)
        local=raster.top_down(tri,textures,1.0,bounds=np.array([2.,2.,8.,8.]),progress=lambda *args:None)
        np.testing.assert_array_equal(local.height,whole.height[2:8,2:8])
        np.testing.assert_array_equal(local.obj,whole.obj[2:8,2:8])
        np.testing.assert_array_equal(local.color,whole.color[2:8,2:8])

if __name__=='__main__': unittest.main()
