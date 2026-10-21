import copy
import tempfile
import unittest
from pathlib import Path

import cv2
import numpy as np
from shapely.geometry import Polygon

import mtscene,raster
from capture_tiles import detect_openings,refine_building,recover_patterned_windows
from solid_city import shell,atlas,audit_shell


class CaptureTilesTest(unittest.TestCase):
    def test_repeated_wide_horizontal_windows_are_not_lost(self):
        image=np.full((400,720,3),180,np.uint8)
        for y in (30,120,210):
            for x in (30,250,470):image[y:y+36,x:x+176]=(30,45,60)
        openings=detect_openings(image,.05)
        wide=[o for o in openings if o['kind']=='window' and o['bounds'][2]-o['bounds'][0]>8.]
        self.assertEqual(len(wide),9)

    def test_outlined_wall_panels_are_not_filled_into_glazing(self):
        image=np.full((400,720,3),180,np.uint8)
        for y in (30,120,210):
            for x in (30,250,470):cv2.rectangle(image,(x,y),(x+176,y+48),(30,45,60),1)
        self.assertEqual(detect_openings(image,.05),[])

    def test_matching_texture_without_observed_row_is_not_repeated(self):
        image=np.full((240,300,3),180,np.uint8)
        for x,y in [(30,30),(100,30),(170,160)]:image[y:y+36,x:x+18]=40
        seeds=[o for o in detect_openings(image,.05) if o['pixel_rect'][1]<60]
        self.assertEqual(len(seeds),2)
        recovered=recover_patterned_windows(image,.05,seeds)
        self.assertTrue(all(o['pixel_rect'][1]<60 for o in recovered))

    def test_observed_staggered_positions_and_door_are_not_snapped_to_grid(self):
        image=np.full((400,400,3),180,np.uint8)
        expected=[]
        for row,y in enumerate((40,120,200)):
            for x in range(30+(25 if row%2 else 0),340,70):
                image[y:y+36,x:x+18]=40;expected.append((x+9,y+18))
        image[344:392,180:200]=25
        openings=detect_openings(image,.05)
        windows=[o for o in openings if o['kind']=='window']
        self.assertEqual(len(windows),len(expected))
        centers=[((o['pixel_rect'][0]+o['pixel_rect'][2])/2,(o['pixel_rect'][1]+o['pixel_rect'][3])/2) for o in windows]
        for point in expected:self.assertLess(min(np.linalg.norm(np.array(point)-c) for c in centers),1.)
        self.assertEqual(sum(o['kind']=='door_candidate' for o in openings),1)

    def test_pattern_recovery_preserves_row_phase_and_never_adds_doors(self):
        image=np.full((300,400,3),180,np.uint8)
        for y,shift in ((30,0),(120,25),(210,0)):
            for x in range(30+shift,350,70):image[y:y+36,x:x+18]=40
        observed=detect_openings(image,.05)
        # Remove one middle detection from each row, leaving visible capture evidence.
        seeds=[o for o in observed if not 95<o['pixel_rect'][0]<150]
        result=recover_patterned_windows(image,.05,seeds)
        self.assertGreater(len(result),len(seeds))
        self.assertTrue(all(o['kind']=='window' for o in result))
        for o in result:
            x,y,X,Y=o['pixel_rect']
            self.assertLess(image[y:Y,x:X].mean(),100)
        self.assertEqual(recover_patterned_windows(np.full_like(image,180),.05,[]),[])

    def test_blank_wall_and_large_shadow_do_not_invent_openings(self):
        image=np.full((400,400,3),150,np.uint8);image[40:360,100:300]=30
        self.assertEqual(detect_openings(image,.05),[])

    def test_upper_storey_or_roof_setback_never_gets_doors(self):
        image=np.full((120,180,3),180,np.uint8)
        for x in (25,75,125):image[68:114,x:x+18]=25
        self.assertTrue(any(o['kind']=='door_candidate' for o in detect_openings(image,.05,0.)))
        upper=detect_openings(image,.05,8.)
        self.assertTrue(upper)
        self.assertTrue(all(o['kind']=='window' for o in upper))

    def test_multi_storey_ornamental_windows_are_not_discarded(self):
        image=np.full((150,230,3),180,np.uint8)
        for x in (10,85,160):
            cv2.rectangle(image,(x,50),(x+40,115),(35,40,45),-1)
            cv2.ellipse(image,(x+20,50),(20,20),0,180,360,(35,40,45),-1)
        windows=detect_openings(image,.1)
        large=[o for o in windows if o['bounds'][3]-o['bounds'][1]>6.]
        self.assertEqual(len(large),3)
        self.assertTrue(all(o['kind']=='window' for o in large))

    def test_generated_recesses_keep_closed_shell_and_original_textures(self):
        meshes=shell(Polygon([(0,0),(12,0),(12,10),(0,10)]),0,12,0)
        textures=[atlas((175,175,175)),atlas((155,155,155))]
        before=copy.deepcopy(textures)
        with tempfile.TemporaryDirectory() as directory:
            result,report=refine_building(meshes,mtscene.SceneObject('test_building',3),textures,Path(directory))
            self.assertGreater(report['counts'].get('window',0),0)
            self.assertFalse(any(audit_shell(result).values()),audit_shell(result))
            self.assertEqual(textures,before)

    def test_arch_shapes_keep_closed_frame_and_wall_seams(self):
        image=np.full((256,256,3),180,np.uint8)
        for y in (10,138):
            for x in (15,143):
                cv2.rectangle(image,(x,y+25),(x+55,y+85),(35,45,55),-1)
                cv2.ellipse(image,(x+27,y+25),(27,25),0,180,360,(35,45,55),-1)
        texture=mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGB_S3TC_DXT1,0,[(256,256,raster.encode_dxt1(image))])
        meshes=shell(Polygon([(0,0),(12,0),(12,10),(0,10)]),0,12,0)
        meshes[0].uvs[:,0]/=meshes[0].uvs[:,0].max()
        meshes[0].uvs[:,1]=1-meshes[0].uvs[:,1]/meshes[0].uvs[:,1].max()
        with tempfile.TemporaryDirectory() as directory:
            result,report=refine_building(meshes,mtscene.SceneObject('arches',3),[texture,texture],Path(directory))
            import json
            catalog=json.loads((Path(directory)/'tiles.json').read_text())
            self.assertTrue(any(p.get('shape')=='arched' for p in catalog['prototypes']))
            self.assertFalse(any(audit_shell(result).values()),audit_shell(result))

    def test_clean_assembly_has_no_capture_material_and_stays_closed(self):
        image=np.full((256,256,3),180,np.uint8)
        for y in (30,100,170):
            for x in (25,85,145,205):image[y:y+35,x:x+20]=(30,45,60)
        texture=mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGB_S3TC_DXT1,0,[(256,256,raster.encode_dxt1(image))])
        meshes=shell(Polygon([(0,0),(12,0),(12,10),(0,10)]),0,12,0)
        meshes[0].uvs[:,0]/=meshes[0].uvs[:,0].max()
        meshes[0].uvs[:,1]=1-meshes[0].uvs[:,1]/meshes[0].uvs[:,1].max()
        textures=[texture,texture]
        with tempfile.TemporaryDirectory() as directory:
            result,report=refine_building(meshes,mtscene.SceneObject('clean',3),textures,Path(directory),clean=True)
            self.assertGreater(report['counts'].get('window',0),0)
            self.assertTrue(all(m.tex_index>=2 for m in result))
            self.assertGreaterEqual(len({m.tex_index for m in result}),4)
            self.assertTrue(all(not np.any(m.uvs) for m in result))
            self.assertFalse(any(audit_shell(result).values()),audit_shell(result))
            self.assertEqual(textures[0].mips[0][2],texture.mips[0][2])

    def test_inpainted_pixels_cannot_create_a_door(self):
        image=np.full((256,256,3),180,np.uint8);image[206:252,80:100]=20
        texture=mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGB_S3TC_DXT1,0,[(256,256,raster.encode_dxt1(image))])
        meshes=shell(Polygon([(0,0),(12,0),(12,10),(0,10)]),0,12,0)
        meshes[0].uvs[:,0]/=meshes[0].uvs[:,0].max()
        meshes[0].uvs[:,1]=1-meshes[0].uvs[:,1]/meshes[0].uvs[:,1].max()
        with tempfile.TemporaryDirectory() as directory:
            observed=Path(directory)/'observed';observed.mkdir()
            inferred=Path(directory)/'inferred';inferred.mkdir()
            _,a=refine_building(meshes,mtscene.SceneObject('doors',3),[texture,texture],observed)
            _,b=refine_building(meshes,mtscene.SceneObject('doors',3),[texture,texture],inferred,{'0':np.zeros((256,256),np.uint8)})
            self.assertGreater(a['counts'].get('door_candidate',0),0)
            self.assertEqual(b['counts'].get('door_candidate',0),0)


if __name__=='__main__':unittest.main()
