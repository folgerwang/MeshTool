"""Capture-bound open construction assemblies; never cap their interior voids."""
import argparse,copy,hashlib,json,shutil
from pathlib import Path
import numpy as np
from shapely.geometry import Polygon,LineString
from solid_city import shell,audit_shell
from building_rules import extrusion
from capture_appearance import bake
from rules_config import snapshot
import mtscene,raster


def reconstruct(profile,own,obj,textures,source,cache):
    src,decoded=cache;oid=own[0].object_id
    source_id=next(i for i,o in enumerate(source.objects) if o.name==obj.name)
    hit=src.obj==source_id
    ortho=raster.top_down(raster.SceneTris(src.pos[hit],src.uv[hit],src.tex[hit],src.obj[hit],src.mesh[hit]),decoded,.15)
    outline=Polygon(profile['rim_xy']);ring=outline.difference(outline.buffer(-profile['rim_width_m'],join_style=2))
    texture=len(textures)
    blank=np.full((4,4,3),150,np.uint8)
    for _ in range(2):textures.append(mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGB_S3TC_DXT1,0,[(4,4,raster.encode_dxt1(blank))]))
    walls=shell(ring,profile['bottom_m'],profile['rim_top_m'],oid,texture)
    walls+=shell(Polygon(profile['deck_xy']),profile['deck_top_m']-.4,profile['deck_top_m'],oid,texture)
    temporary=mtscene.Group(textures,walls,[copy.copy(o) for o in source.objects])
    # Object IDs belong to the generated scene, not the segmented source.
    temporary.objects=[mtscene.SceneObject('unused_'+str(i),0) for i in range(oid+1)];temporary.objects[oid]=obj
    bake(temporary,source,only={obj.name},source_cache=cache)
    result=temporary.meshes
    steel=np.full((4,4,3),profile['steel_rgb'],np.uint8);steel_id=len(textures)
    textures.append(mtscene.Texture(raster.GL_COMPRESSED_RGB_S3TC_DXT1,raster.GL_COMPRESSED_RGB_S3TC_DXT1,0,[(4,4,raster.encode_dxt1(steel))]))
    measured=[]
    for a,b in profile['beams_xy']:
        a,b=np.asarray(a),np.asarray(b);samples=[]
        for t in np.linspace(.05,.95,60):
            x,y=a*(1-t)+b*t;i=int((ortho.max_y-y)/ortho.res);j=int((x-ortho.min_x)/ortho.res)
            patch=ortho.height[max(0,i-3):i+4,max(0,j-3):j+4]
            values=patch[np.isfinite(patch)&(patch>profile['beam_height_range_m'][0])&(patch<profile['beam_height_range_m'][1])]
            if len(values):samples.append(np.median(values))
        z=float(np.median(samples)) if samples else profile['beam_fallback_top_m']
        measured.append(z);width=profile['beam_width_m'];depth=profile['beam_depth_m'];flange=.10
        for w,low,high in [(width,z-flange,z),(.15,z-depth+flange,z-flange),(width,z-depth,z-depth+flange)]:
            poly=LineString([a,b]).buffer(w/2,cap_style=2,join_style=2)
            points=extrusion(poly,low,high);center=points.mean(0)
            result.append(mtscene.Mesh(steel_id,oid,center,(points-center).astype(np.float32),np.zeros((len(points),2),np.float32),None,[(4,np.arange(len(points),dtype=np.uint32))],0,0))
    return result,dict(building=obj.name,status='capture_measured_open_structure',counts={'steel_beams':len(measured),'deck':1,'retaining_ring':1},roof=[],glass_meshes=0,
        structure='Open construction pit, measured I-beam layout; no solid roof cap',beam_top_heights_m=measured,
        limitations=['Frame layout measured from capture; small joints omitted','Terrain below the pit remains unchanged; excavation depth is not reconstructed'])


def run(candidate,source,profile_path,output):
    candidate,source,profile_path,output=map(Path,(candidate,source,profile_path,output))
    profile=json.loads(profile_path.read_text())
    assert hashlib.sha256(source.read_bytes()).hexdigest()==profile['source_sha256']
    shutil.copytree(candidate,output,ignore=shutil.ignore_patterns('city.mtscene','verification.json','review_index.json','pcg.mtscene','capture.mtscene','profile.json','visual_findings.json'))
    scene=mtscene.load(candidate/'city.mtscene');original=mtscene.load(source);report=None
    for bi,b in enumerate(scene):
        for gi,g in enumerate(b.groups):
            for oid,obj in enumerate(g.objects):
                if obj.name!=profile['building']:continue
                sg=original[bi].groups[gi];cache=(raster.collect(sg),raster.decode_textures(sg,size=512))
                own=[m for m in g.meshes if m.object_id==oid]
                replacement,report=reconstruct(profile,own,obj,g.textures,sg,cache)
                g.meshes=[m for m in g.meshes if m.object_id!=oid]+replacement
    assert report is not None
    catalog=output/profile['building']/'tiles.json'
    if catalog.exists():catalog.rename(catalog.with_name('previous_tiles.json'))
    shutil.copy2(profile_path,output/'measured_profile_snapshot'/profile_path.name)
    manifest=json.loads((output/'manifest.json').read_text())
    manifest['structure_parent']=dict(path=str(candidate.resolve()),sha256=hashlib.sha256((candidate/'city.mtscene').read_bytes()).hexdigest())
    manifest['buildings']=[report if r['building']==profile['building'] else r for r in manifest['buildings']]
    manifest['config']=snapshot(output);manifest['config']['profile_hashes']={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (output/'measured_profile_snapshot').glob('*.json')}
    mtscene.save(output/'city.mtscene',scene);(output/'manifest.json').write_text(json.dumps(manifest,indent=2))


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('candidate');p.add_argument('source');p.add_argument('profile');p.add_argument('output');a=p.parse_args();run(a.candidate,a.source,a.profile,a.output)
