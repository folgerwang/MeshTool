import unittest
import numpy as np
from facade_layout import lattice,pattern,finish,details,wall_mask,procedural_tiles
from capture_tiles import detect_openings,recover_patterned_windows


class FacadeLayoutTest(unittest.TestCase):
    def test_lattice_missing_bays_and_outlier(self):
        fit=lattice([1,3,5,9,11,14.31])
        self.assertAlmostEqual(fit['spacing_m'],2,places=2)
        self.assertEqual(fit['support'],5)

    def test_staggered_cycle_has_independent_row_phase(self):
        openings=[]
        for row in range(8):
            for column in range(5):
                x=1+column*2+(row%2)
                openings.append(dict(kind='window',bounds=[x,row*3,x+1,row*3+1.5]))
        result=pattern(openings)
        self.assertEqual(result['row_cycle']['rows'],2)
        self.assertEqual(result['row_cycle']['offsets_bays'],[0.,.5])
        self.assertAlmostEqual(result['floor']['spacing_m'],3)

    def test_blank_gap_is_not_filled_by_repetition(self):
        image=np.full((180,400,3),180,np.uint8)
        for x in (30,90,210,270,330):image[60:96,x:x+18]=40
        seeds=detect_openings(image,.05)
        recovered=recover_patterned_windows(image,.05,seeds)
        self.assertEqual(len(recovered),len(seeds))

    def test_wall_mark_is_separate_and_openings_are_excluded(self):
        image=np.full((200,240,3),(186,170,150),np.uint8)
        image[70:95,90:125]=(180,50,40)
        image[110:150,160:180]=(30,60,160)
        openings=[dict(pixel_rect=[160,110,180,150])]
        mask=wall_mask(image,np.ones(image.shape[:2],bool),openings,.05)
        marks=details(image,mask,.05)
        self.assertEqual(len(marks),1)
        self.assertEqual(marks[0]['rgb'],[180,50,40])
        self.assertEqual(finish(image,mask,.05)['rgb'],[186,170,150])
        self.assertEqual(details(image,np.zeros_like(mask),.05),[])

    def test_tile_joint_detection_and_synthesis(self):
        image=np.full((400,400,3),(185,160,140),np.uint8)
        image[::20]=(100,90,80);image[:,::20]=(100,90,80)
        result=finish(image,np.ones(image.shape[:2],bool),.025)
        self.assertEqual(result['kind'],'tile_candidate')
        for joint in result['joints'].values():self.assertAlmostEqual(joint['spacing'],.5,delta=.04)
        texture=procedural_tiles(result,image.shape,.025)
        self.assertEqual(texture.shape,image.shape)
        self.assertGreater(len(np.unique(texture.reshape(-1,3),axis=0)),1)

    def test_separate_wall_detail_mesh_keeps_shell_closed(self):
        import json
        import tempfile
        from pathlib import Path
        from shapely.geometry import Polygon
        import mtscene,raster
        from capture_tiles import refine_building
        from solid_city import shell,audit_shell
        image=np.full((256,256,3),(185,170,150),np.uint8)
        image[80:110,90:140]=(180,50,40)
        texture=mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGB_S3TC_DXT1,0,[(256,256,raster.encode_dxt1(image))])
        meshes=shell(Polygon([(0,0),(12,0),(12,10),(0,10)]),0,12,0)
        meshes[0].uvs[:,0]/=meshes[0].uvs[:,0].max()
        meshes[0].uvs[:,1]=1-meshes[0].uvs[:,1]/meshes[0].uvs[:,1].max()
        with tempfile.TemporaryDirectory() as directory:
            generated,report=refine_building(meshes,mtscene.SceneObject('marks',3),[texture,texture],Path(directory),clean=True)
            catalog=json.loads((Path(directory)/'tiles.json').read_text())
            self.assertGreater(report['counts'].get('wall_detail',0),0)
            self.assertTrue(any(r['part']=='wall_detail' for r in catalog['semantic_meshes']))
            self.assertFalse(any(audit_shell(generated).values()))
            self.assertTrue(all(m.tex_index>=2 for m in generated))

    def test_elevated_door_requires_measured_landing(self):
        import json
        import tempfile
        from pathlib import Path
        from shapely.geometry import Polygon
        import mtscene,raster
        from capture_tiles import refine_building
        from solid_city import shell
        image=np.full((256,256,3),180,np.uint8)
        for x in (40,120,200):image[160:244,x:x+20]=25
        texture=mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGB_S3TC_DXT1,0,[(256,256,raster.encode_dxt1(image))])
        upper=shell(Polygon([(0,0),(12,0),(12,10),(0,10)]),6,12,0)
        upper[0].uvs[:,0]/=upper[0].uvs[:,0].max()
        upper[0].uvs[:,1]=1-upper[0].uvs[:,1]/upper[0].uvs[:,1].max()
        lower=shell(Polygon([(20,0),(23,0),(23,3),(20,3)]),0,3,0)
        for support,expected in ((None,False),({0:[6.]},True)):
            with tempfile.TemporaryDirectory() as directory:
                refine_building(upper+lower,mtscene.SceneObject('patio',3),[texture,texture],Path(directory),clean=True,support_levels=support)
                catalog=json.loads((Path(directory)/'tiles.json').read_text())
                doors=[i for i in catalog['instances'] if i.get('kind')=='door_candidate' and i['face_id']==0]
                self.assertEqual(bool(doors),expected,[(i.get('kind'),i.get('bounds')) for i in catalog['instances'] if i['face_id']==0])
                for door in doors:
                    self.assertEqual(door['support_level_z'],6.)
                    self.assertEqual(door['semantic_status'],'inferred_patio_opening')


if __name__=='__main__':unittest.main()
