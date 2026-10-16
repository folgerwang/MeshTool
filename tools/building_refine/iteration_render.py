"""Native, identically framed capture/before/candidate renders for each iteration."""
import argparse
import json
import hashlib
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from PIL import Image,ImageDraw,ImageChops
import mtscene
from building_review import isolate


def render(baseline,candidate,output,names,workers=1,roof=False,no_before=False,capture_only=False,reuse_captures=False):
    baseline,candidate,output=map(lambda p:Path(p).resolve(),(baseline,candidate,output))
    output.mkdir(parents=True,exist_ok=True)
    scene=mtscene.load(candidate/'city.mtscene')
    manifest=json.loads((candidate/'manifest.json').read_text())
    reports={r['building']:r for r in manifest['buildings']}
    executable=Path(__file__).resolve().parents[2]/'build/Release/MeshTool.exe'
    startup=subprocess.STARTUPINFO();startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=0
    views=[(30,-20),(210,-20)]+([(120,-65)] if roof else [])
    def render_one(name):
        folder=output/name;folder.mkdir(exist_ok=True)
        candidate_box=None if capture_only else isolate(scene,name,folder/'candidate.mtscene')
        profile=json.loads((baseline/name/'profile.json').read_text())
        bounds=profile['pcg_bounds'] or profile['capture_bounds']
        paths={'capture':baseline/name/'capture.mtscene','before':baseline/name/'pcg.mtscene','candidate':folder/'candidate.mtscene'}
        if reports[name].get('source_members'):
            profile=json.loads((candidate/name/'profile.json').read_text())
            paths['capture']=candidate/name/'capture.mtscene'
            bounds=profile['pcg_bounds'] or profile['capture_bounds']
        if no_before:del paths['before']
        if capture_only:paths={'capture':paths['capture']}
        capture_hash=hashlib.sha256(paths['capture'].read_bytes()).hexdigest()
        cache=folder/'capture_record.json'
        expected=dict(scene_sha256=capture_hash,bounds=bounds,views=[list(v) for v in views])
        cache_valid=reuse_captures and cache.exists() and json.loads(cache.read_text())==expected
        evidence=[]
        for variant,path in paths.items():
            for yaw,pitch in views:
                prefix=folder/f'{variant}-{yaw}'
                image=Path(str(prefix)+'_original.png')
                if (variant=='candidate' and candidate_box is None) or (variant=='before' and not path.exists()):
                    reason=reports[name].get('classification_correction',{}).get('reason','No generated geometry')
                    with Image.open(folder/f'capture-{yaw}_original.png') as source_image:
                        blank=Image.new('RGB',source_image.size,source_image.convert('RGB').getpixel((0,0)))
                    label=ImageDraw.Draw(blank);x,y=int(blank.width*.25),int(blank.height*.5)
                    label.text((x,y),'No PCG building: classification corrected' if variant=='candidate' else 'Before: no generated building',fill='white')
                    label.text((x,y+30),reason,fill='white');blank.save(image)
                    evidence.append(str(image));continue
                if variant=='capture' and cache_valid and image.exists():
                    Image.open(image).verify();evidence.append(str(image));continue
                args=[str(executable),str(path),'--shots',str(prefix),'--yaw',str(yaw),'--pitch',str(pitch),'--zoom','1.0','--box',*map(str,bounds)]
                run=subprocess.run(args,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=60,startupinfo=startup)
                prefix.with_suffix('.log').write_bytes(run.stdout+run.stderr)
                image=Path(str(prefix)+'_original.png')
                if run.returncode or not image.exists():raise RuntimeError(f'Render failed: {name} {variant} {yaw}')
                evidence.append(str(image))
        cache.write_text(json.dumps(expected,indent=2))
        sheet=Image.new('RGB',(500*len(paths),370*len(views)),(25,27,32));draw=ImageDraw.Draw(sheet)
        for row,(yaw,pitch) in enumerate(views):
            originals={v:Image.open(folder/f'{v}-{yaw}_original.png').convert('RGB') for v in paths}
            rectangles=[]
            for im in originals.values():
                bg=Image.new('RGB',im.size,im.getpixel((0,0)))
                rect=ImageChops.difference(im,bg).convert('L').point(lambda p:255 if p>12 else 0).getbbox()
                if rect:rectangles.append(rect)
            crop=(max(0,min(r[0] for r in rectangles)-12),max(0,min(r[1] for r in rectangles)-12),
                  min(im.width,max(r[2] for r in rectangles)+12),min(im.height,max(r[3] for r in rectangles)+12)) if rectangles else (0,0,im.width,im.height)
            for col,variant in enumerate(paths):
                im=originals[variant].crop(crop);im.thumbnail((500,340))
                sheet.paste(im,(col*500,row*370+25));draw.text((col*500+12,row*370+8),f'{name}: {variant}, yaw {yaw}',fill='white')
        sheet.save(folder/'comparison.jpg')
        (folder/'render_record.json').write_text(json.dumps(dict(building=name,baseline=str(baseline),candidate=str(candidate),bounds=bounds,views=views,images=evidence,candidate_render='native' if candidate_box is not None else 'not_rendered_no_geometry',visual_review='pending'),indent=2))
        print(name,'comparison ready',flush=True)
    with ThreadPoolExecutor(max_workers=workers) as pool:
        list(pool.map(render_one,names))


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('baseline');p.add_argument('candidate');p.add_argument('output');p.add_argument('--only');p.add_argument('--workers',type=int,choices=range(1,5),default=1);p.add_argument('--roof',action='store_true');p.add_argument('--no-before',action='store_true');p.add_argument('--capture-only',action='store_true');p.add_argument('--reuse-captures',action='store_true');a=p.parse_args()
    names=a.only.split(',') if a.only else [b['building'] for b in json.loads((Path(a.candidate)/'manifest.json').read_text())['buildings']]
    render(a.baseline,a.candidate,a.output,names,a.workers,a.roof,a.no_before,a.capture_only,a.reuse_captures)
