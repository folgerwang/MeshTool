import copy,unittest
from collections import Counter
import numpy as np
from shapely.geometry import Polygon
import mtscene,raster,refine,building
from solid_city import shell,atlas,audit_shell
from capture_appearance import bake,faces
from test_pcg import fingerprint

class CaptureAppearanceTest(unittest.TestCase):
 def test_roof_levels_are_projected_separately(self):
  a=shell(Polygon([(0,0),(4,0),(4,4),(0,4)]),0,10,0)[1]
  b=shell(Polygon([(5,0),(9,0),(9,4),(5,4)]),0,30,0)[1]
  points=np.concatenate([a.vertices+a.translation,b.vertices+b.translation])
  m=mtscene.Mesh(0,0,np.zeros(3),points,None,None,[(4,np.arange(len(points),dtype=np.uint32))],0,0)
  roofs=[f for f,_ in faces(m) if f.normal[2]>.5]
  self.assertEqual(sorted(round(f.origin[2]) for f in roofs),[10,30])
  for f in roofs:self.assertLess(np.ptp(f.tris[:,:,2]),.001)

 def test_capture_layout_and_nearest_wall_selection(self):
  # Asymmetric doorway plus alternate half-window rows; no repeating UV reset.
  image=np.full((512,512,3),180,np.uint8)
  for row in range(4):
   for col in range(4):
    x=col*112+(48 if row%2 else 8)
    image[row*112+8:row*112+80,x:x+48]=(35,70,110)
  image[420:500,290:340]=(120,40,20)
  points=np.array([[[0,0,.2],[1,0,.2],[1,1,.2]],[[0,0,.2],[1,1,.2],[0,1,.2]]])
  distant=points.copy();distant[:,:,2]=4
  pos=np.concatenate([points,distant]);uv=pos[:,:,:2].copy()
  src=raster.SceneTris(pos,uv,np.array([0,0,1,1]),np.zeros(4,dtype=int),np.zeros(4,dtype=int))
  textures=np.stack([image,np.full_like(image,(220,20,40))])
  face=building.Face(points,np.zeros(3),np.array([1.,0,0]),np.array([0.,1,0]),np.array([0.,0,1]),(1.,1.),'wall')
  color,_,covered=refine.project_face(face,src,textures,1/512,(-6,6),nearest_plane=True)
  self.assertTrue(covered.all())
  np.testing.assert_array_equal(color,image[::-1])

 def test_projection_changes_appearance_without_geometry_damage(self):
  meshes=shell(Polygon([(0,0),(12,0),(12,10),(0,10)]),0,12,0)
  image=np.full((16,16,3),(60,90,150),np.uint8)
  image[4:12,4:12]=0  # Dark captured windows must not be filled as missing data.
  texture=mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGB_S3TC_DXT1,0,[(16,16,raster.encode_dxt1(image))])
  source=mtscene.Group([texture,texture],copy.deepcopy(meshes),[mtscene.SceneObject('building',3)])
  target=mtscene.Group([atlas((160,160,160)),texture],copy.deepcopy(meshes),copy.deepcopy(source.objects))
  before=list(map(fingerprint,source.meshes))
  def triangles(group):
   return Counter(tuple(np.round((m.vertices[idx]+m.translation).ravel(),5)) for m in group.meshes for _,indices in m.draw_calls for idx in indices.reshape(-1,3))
  geometry=triangles(target);report=bake(target,source,.25)
  self.assertEqual(geometry,triangles(target));self.assertFalse(any(audit_shell(target.meshes).values()))
  self.assertEqual(before,list(map(fingerprint,source.meshes)))
  self.assertGreater(report[0]['wall_capture_coverage'],.999)
 def test_face_partition_includes_every_triangle_once(self):
  for mesh in shell(Polygon([(0,0),(12,0),(12,10),(0,10)]),0,12,0):
   indices=np.concatenate([idx for face,idx in faces(mesh)])
   np.testing.assert_array_equal(np.sort(indices),np.sort(mesh.draw_calls[0][1]))
