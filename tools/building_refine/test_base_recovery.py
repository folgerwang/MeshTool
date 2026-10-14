import unittest
import numpy as np
import mtscene, base_recovery, regular_facade, facade
from test_pcg import fingerprint


def mesh(points,oid):
 p=np.array(points,np.float32)
 return mtscene.Mesh(0xffffffff,oid,np.zeros(3),p,None,None,[(4,np.arange(len(p),dtype=np.uint32))])

def wall(x0,x1,z0,z1,y=0):
 return [[x0,y,z0],[x1,y,z0],[x1,y,z1],[x0,y,z0],[x1,y,z1],[x0,y,z1]]

class BaseRecoveryTest(unittest.TestCase):
 def fixture(self):
  own=mesh(wall(0,12,5,30),0)
  fragment=mesh(wall(0,5,0,5),1)
  ground=mesh([[-5,-5,0],[20,-5,0],[20,5,0],[-5,-5,0],[20,5,0],[-5,5,0]],2)
  unrelated=mesh(wall(0,12,0,30,y=4),3)
  group=mtscene.Group([], [own,fragment,ground,unrelated],[mtscene.SceneObject('tower',3),mtscene.SceneObject('fragment',3),mtscene.SceneObject('ground',1),mtscene.SceneObject('neighbor',3)])
  return own,group
 def test_context_fills_lower_wall_without_absorbing_neighbor_or_mutating_capture(self):
  own,g=self.fixture();before=[fingerprint(m) for m in g.meshes]
  plan=base_recovery.infer([own],g)
  self.assertAlmostEqual(plan['ground_z'],0.)
  self.assertEqual(len(plan['planes']),1)
  p=plan['planes'][0]
  self.assertEqual(p['donor_objects'],[1]);self.assertAlmostEqual(p['observed_area'],25.)
  self.assertAlmostEqual(p['added_area'],60.)
  _,planes,stats=regular_facade.rebuild([own],recovery=plan)
  xyz=np.concatenate([np.array(p['tri']).reshape(-1,3) for p in planes])
  self.assertAlmostEqual(xyz[:,2].min(),0.)
  self.assertLess(np.ptp(xyz[:,1]),.01)
  self.assertAlmostEqual(stats['recovered_base_area'],60.)
  self.assertEqual(before,[fingerprint(m) for m in g.meshes])
 def test_no_ground_evidence_does_not_guess_base(self):
  own,g=self.fixture();g.meshes=[g.meshes[0],g.meshes[1],g.meshes[3]]
  self.assertEqual(base_recovery.infer([own],g),{})
 def test_apply_preserves_neighbors_and_archives_original_on_reapply(self):
  own,g=self.fixture();before=fingerprint(own);neighbors=[fingerprint(m) for m in g.meshes[1:]]
  batches=[mtscene.Batch(False,False,False,(0.,0.),[g])]
  for _ in range(2):
   self.assertEqual(facade.apply(batches,(0,0,0)),(1,1))
   self.assertEqual(neighbors,[fingerprint(m) for m in g.meshes if m.object_id!=0])
   originals=[m for m in g.meshes if m.object_id==0 and m.model_variant==1]
   self.assertEqual(len(originals),1)
   import copy
   restored=copy.copy(originals[0]);restored.model_variant=0
   self.assertEqual(before,fingerprint(restored))
   generated=[m for m in g.meshes if m.object_id==0 and m.model_variant==2]
   self.assertLess(min((m.vertices+m.translation)[:,2].min() for m in generated),.1)
 def test_existing_gap_between_wings_is_not_bridged(self):
  own,g=self.fixture();own=mesh(wall(0,4,5,30)+wall(8,12,5,30),0);g.meshes[0]=own;g.meshes[1]=mesh(wall(0,4,0,5),1)
  plan=base_recovery.infer([own],g)
  _,planes,_=regular_facade.rebuild([own],recovery=plan)
  for p in planes:
   xyz=np.array(p['tri'])
   centers=xyz.mean(1)
   self.assertFalse(((centers[:,0]>4.01)&(centers[:,0]<7.99)).any())

if __name__=='__main__':unittest.main()
