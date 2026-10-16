"""Batch PCG buildings with per-object checkpoints and explicit procedural fallback."""
import argparse,copy,json,time,hashlib
from pathlib import Path
import numpy as np
import shapely
from shapely.geometry import MultiPoint
import mtscene,facade,building
from compose_terrain import signature


def proxy_model(meshes,oid):
    points=np.concatenate([m.vertices.astype(float)+m.translation for m in meshes])
    xy=np.unique(np.round(points[:,:2],2),axis=0)
    hull=shapely.concave_hull(MultiPoint(xy),ratio=.25,allow_holes=False)
    if hull.geom_type!='Polygon' or hull.area<.05:hull=MultiPoint(xy).convex_hull.buffer(.15)
    hull=hull.simplify(.25,preserve_topology=True)
    bottom=float(points[:,2].min());top=float(np.percentile(points[:,2],98))
    top=max(bottom+.15,top)
    model=building.BuildingModel(bottom,hull,[building.RoofPart(hull,np.array([0.,0.,top]))])
    faces=building.model_faces(model)
    pos=np.concatenate([f.tris.reshape(-1,3) for f in faces]);center=pos.mean(0)
    mesh=mtscene.Mesh(0xffffffff,oid,center,(pos-center).astype(np.float32),None,None,[(4,np.arange(len(pos),dtype=np.uint32))],3,2)
    return mesh


def run(source_path,library_path,out):
    out=Path(out);out.mkdir(parents=True,exist_ok=True)
    provenance=dict(source_sha256=hashlib.sha256(Path(source_path).read_bytes()).hexdigest(),
                    library_sha256=hashlib.sha256(Path(library_path).read_bytes()).hexdigest())
    run_file=out/'run.json'
    if run_file.exists() and json.loads(run_file.read_text())!=provenance:
        raise ValueError('Checkpoint directory belongs to different source inputs')
    if not run_file.exists() and (out/'manifest.json').exists():
        prior=json.loads((out/'manifest.json').read_text())
        if prior['source_sha256']!=provenance['source_sha256']:raise ValueError('Checkpoint source mismatch')
    run_file.write_text(json.dumps(provenance,indent=2))
    source=mtscene.load(source_path);library=mtscene.load(library_path)
    report=[];start=time.time()
    for bi,batch in enumerate(source):
      for gi,group in enumerate(batch.groups):
        lg=library[bi].groups[gi]
        for oid,obj in enumerate(group.objects):
          if obj.cls!=3:continue
          own=[m for m in group.meshes if m.object_id==oid]
          if not own:continue
          file=out/f'{bi}_{gi}_{oid}.mtscene';meta=out/f'{bi}_{gi}_{oid}.json'
          if file.exists() and meta.exists():report.append(json.loads(meta.read_text()));continue
          record=dict(batch=bi,group=gi,object=oid,name=obj.name)
          existing=[m for m in lg.meshes if m.object_id==oid and m.model_variant==2]
          if existing:
            archive=[m for m in lg.meshes if m.object_id==oid and m.model_variant==1]
            if sorted(map(signature,archive))!=sorted(map(signature,own)):raise ValueError('Existing PCG source mismatch')
            new=copy.deepcopy(existing);textures=lg.textures;record['method']='existing_refined'
          else:
            base=len(group.textures)
            try:
              new,extra,stats=facade.build(own,oid,base,group.textures)
              if not new or not stats.get('windows'):raise ValueError('No usable facade modules')
              textures=group.textures+extra;record.update(method='capture_fitted_facade',windows=int(stats['windows']))
            except (ValueError,shapely.errors.GEOSException) as error:
              proxy=proxy_model(own,oid)
              try:
                new,extra,stats=facade.build([proxy],oid,0,None)
                if not new or not stats.get('windows'):raise ValueError('Fragment too small for windows')
                textures=extra;record.update(method='procedural_footprint_facade',windows=int(stats['windows']))
              except (ValueError,shapely.errors.GEOSException):
                new=[proxy];textures=[];record.update(method='procedural_fragment',windows=0)
              record['fallback_reason']=str(error)
          used=sorted({m.tex_index for m in new if m.tex_index!=0xffffffff});remap={t:i for i,t in enumerate(used)}
          compact=[]
          for m in new:
            mesh=copy.copy(m);mesh.object_id=0;mesh.model_variant=2
            if mesh.tex_index!=0xffffffff:mesh.tex_index=remap[mesh.tex_index]
            if not np.isfinite(mesh.vertices).all():raise ValueError('Nonfinite generated model')
            compact.append(mesh)
          asset=mtscene.Scene([mtscene.Batch(False,True,batch.is_georeferenced,batch.reference_pos,[mtscene.Group([textures[i] for i in used],compact,[copy.copy(obj)])])])
          record.update(meshes=len(compact),vertices=sum(len(m.vertices) for m in compact),asset=file.name)
          mtscene.save(file,asset);meta.write_text(json.dumps(record,indent=2));report.append(record)
          print(f'{len(report)}/602 {obj.name}: {record["method"]}, {record["vertices"]} vertices ({time.time()-start:.0f}s)',flush=True)
    (out/'manifest.json').write_text(json.dumps(dict(source=str(Path(source_path).resolve()),source_sha256=hashlib.sha256(Path(source_path).read_bytes()).hexdigest(),buildings=report),indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('source');p.add_argument('library');p.add_argument('output');a=p.parse_args();run(a.source,a.library,a.output)
