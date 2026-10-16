"""Export capture-bound per-building profiles and source/PCG review scenes.

Automatic measurements remain pending visual review; this never labels them
as approved or copies the calibrated landmark design to another building.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import numpy as np
from PIL import Image, ImageDraw
import mtscene


def isolate(scene, name, path, members=None):
    batches = []
    bounds = []
    for batch in scene:
        groups = []
        for group in batch.groups:
            ids = {i for i,o in enumerate(group.objects) if o.name in (members or [name])}
            own = [copy.copy(m) for m in group.meshes if m.object_id in ids]
            if not own:
                continue
            used = sorted({m.tex_index for m in own if 0 <= m.tex_index < len(group.textures)})
            remap = {old:new for new,old in enumerate(used)}
            for m in own:
                m.object_id = 0; m.tex_index = remap.get(m.tex_index, m.tex_index)
                bounds.append(m.vertices.astype(float)+m.translation)
            groups.append(mtscene.Group([group.textures[i] for i in used],own,[mtscene.SceneObject(name,3)]))
        if groups:
            b = copy.copy(batch); b.groups = groups; batches.append(b)
    if not bounds:
        return None
    mtscene.save(path,mtscene.Scene(batches))
    points = np.concatenate(bounds)
    return [*points.min(0).tolist(),*points.max(0).tolist()]


def run(directory, source):
    directory = Path(directory)
    source_hash = hashlib.sha256(Path(source).read_bytes()).hexdigest()
    manifest = json.loads((directory/'manifest.json').read_text())
    original = mtscene.load(source); pcg = mtscene.load(directory/'city.mtscene')
    records = []
    for entry in manifest['buildings']:
        name = entry['building']; folder = directory/name; folder.mkdir(exist_ok=True)
        capture_box = isolate(original,name,folder/'capture.mtscene',entry.get('source_members'))
        pcg_box = isolate(pcg,name,folder/'pcg.mtscene')
        record = dict(building=name,source_capture_sha256=source_hash,
                      review_status='pending_visual_review',capture_bounds=capture_box,
                      pcg_bounds=pcg_box,roof=entry.get('roof',[]),
                      source_scope='Matching labeled source object; adjoining fragments may contain additional evidence')
        catalog_path = folder/'tiles.json'
        if catalog_path.exists() and entry.get('status')!='excluded_nonbuilding_capture':
            catalog = json.loads(catalog_path.read_text())
            record['faces'] = catalog['faces']
            record['openings'] = [i for i in catalog['instances'] if i.get('kind')]
            record['window_styles'] = [{k:v for k,v in p.items() if k!='geometry'} for p in catalog['prototypes'] if p.get('kind')!='wall']
            record['measured_internal_members'] = sum(len(p.get('captured_members',[])) for p in catalog['prototypes'])
            record['unresolved_faces'] = [f['face_id'] for f in catalog['faces'] if not f['openings']]
            # Registered capture and detection overlay, largest faces first.
            candidates = sorted(folder.glob('*_capture.png'),key=lambda p:Image.open(p).width*Image.open(p).height,reverse=True)[:6]
            sheet = Image.new('RGB',(1200,80+260*len(candidates)),(25,27,32)); draw=ImageDraw.Draw(sheet)
            draw.text((15,15),f'{name}: captured facade | measured opening overlay (NOT final approval)',fill='white')
            for row,p in enumerate(candidates):
                for col,q in enumerate([p,p.with_name(p.name.replace('_capture','_detections'))]):
                    im=Image.open(q).convert('RGB'); im.thumbnail((580,230))
                    sheet.paste(im,(10+600*col,70+260*row));draw.text((10+600*col,50+260*row),q.stem,fill='white')
            sheet.save(folder/'facade_review.jpg')
        else:
            record['status'] = entry.get('status')
        (folder/'profile.json').write_text(json.dumps(record,indent=2))
        records.append({k:v for k,v in record.items() if k not in ('faces','openings','window_styles')})
        print(name,'review assets saved',flush=True)
    (directory/'review_index.json').write_text(json.dumps(records,indent=2))
    assert hashlib.sha256(Path(source).read_bytes()).hexdigest()==source_hash


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('directory');p.add_argument('source');a=p.parse_args()
    run(a.directory,a.source)
