import copy,tempfile,unittest
from pathlib import Path
import numpy as np
import mtscene
from iteration_loop import building_hashes


class IterationHashTest(unittest.TestCase):
    def test_unrelated_changes_and_texture_remaps_do_not_invalidate_review(self):
        tex=mtscene.Texture(1,1,0,[(1,1,b'abc')])
        mesh=mtscene.Mesh(0,0,np.zeros(3),np.array([[0,0,0],[1,0,0],[0,1,0]],np.float32),None,None,[(4,np.array([0,1,2],np.uint32))])
        other=copy.deepcopy(mesh);other.object_id=1
        group=mtscene.Group([tex],[mesh,other],[mtscene.SceneObject('A',3),mtscene.SceneObject('B',3)])
        scene=mtscene.Scene([mtscene.Batch(False,False,False,(0.,0.),[group])])
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'test.mtscene';mtscene.save(path,scene);before=building_hashes(path)
            group.textures.insert(0,mtscene.Texture(1,1,0,[(1,1,b'xyz')]))
            mesh.tex_index=1;other.tex_index=1;other.vertices[0,0]=2
            mtscene.save(path,scene);after=building_hashes(path)
            self.assertEqual(before['A'],after['A']);self.assertNotEqual(before['B'],after['B'])
            group.textures[1].mips=[(1,1,b'123')];mtscene.save(path,scene)
            self.assertNotEqual(after['A'],building_hashes(path)['A'])


if __name__=='__main__':unittest.main()
