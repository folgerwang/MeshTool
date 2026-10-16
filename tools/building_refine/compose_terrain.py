"""Add saved PCG buildings to a separate terrain scene, without captured fallbacks.

python compose_terrain.py terrain/manifest.json full_scene_refined.mtscene NEW_DIRECTORY
The asset library must retain the source batch/group/object indices and originals.
World coordinates are preserved; inferred terrain never moves measured buildings.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path

import numpy as np
import mtscene
from terrain_scene import original_group


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def signature(mesh):
    h=hashlib.sha256()
    h.update(str((mesh.tex_index,mesh.object_id,mesh.material)).encode())
    for value in (mesh.translation,mesh.vertices,mesh.uvs,mesh.colors):
        h.update(b'none' if value is None else value.tobytes())
    for primitive,indices in mesh.draw_calls:
        h.update(str(primitive).encode());h.update(indices.tobytes())
    return h.digest()


def compose(manifest_path,library_path,destination):
    manifest_path=Path(manifest_path).resolve();library_path=Path(library_path).resolve()
    destination=Path(destination).resolve()
    if destination.exists():raise ValueError('Use a new output directory; existing scenes are protected')
    manifest=json.loads(manifest_path.read_text())
    source_path=Path(manifest['source']);terrain_path=manifest_path.parent/manifest['scene']
    hashes={str(p):digest(p) for p in (source_path,terrain_path,library_path)}
    if hashes[str(source_path)]!=manifest['source_sha256']:raise ValueError('Terrain source fingerprint mismatch')
    source=mtscene.load(source_path);result=mtscene.load(terrain_path);library=mtscene.load(library_path)
    added=[];pending=[]
    for record in manifest['groups']:
        bi,gi=record['source_batch'],record['source_group']
        sb,lb=source[bi],library[bi]
        if (sb.is_georeferenced,sb.reference_pos)!=(lb.is_georeferenced,lb.reference_pos):
            raise ValueError('PCG library geographic reference mismatch')
        target_batch=result[record['output_batch']]
        if (sb.is_georeferenced,sb.reference_pos)!=(target_batch.is_georeferenced,target_batch.reference_pos):
            raise ValueError('Terrain geographic reference mismatch')
        sg=original_group(sb.groups[gi]);lg=lb.groups[gi]
        target=target_batch.groups[record['output_group']]
        if any(o.cls==3 for o in target.objects):raise ValueError('Expected terrain-only destination')
        texture_map={}
        for placement in record['placements']:
            if placement['kind']!='building':continue
            oid=placement['source_object']
            if oid>=len(lg.objects) or lg.objects[oid]!=sg.objects[oid]:
                raise ValueError(f'PCG library object identity mismatch at {bi}:{gi}:{oid}')
            refined=[m for m in lg.meshes if m.object_id==oid and m.model_variant==2]
            if not refined:
                pending.append(dict(batch=bi,group=gi,object=oid,name=placement['name']))
                continue
            originals=[m for m in lg.meshes if m.object_id==oid and m.model_variant==1]
            captured=[m for m in sg.meshes if m.object_id==oid]
            if not originals or sorted(map(signature,originals))!=sorted(map(signature,captured)):
                raise ValueError(f'PCG originals do not match terrain source: {placement["name"]}')
            for mesh in originals:
                if mesh.tex_index!=0xffffffff and lg.textures[mesh.tex_index]!=sg.textures[mesh.tex_index]:
                    raise ValueError('PCG source texture mismatch')
            new_id=len(target.objects);target.objects.append(copy.copy(lg.objects[oid]))
            for mesh in refined:
                if not np.isfinite(mesh.vertices).all() or not np.isfinite(mesh.translation).all():
                    raise ValueError('Non-finite PCG geometry')
                new=copy.copy(mesh);new.object_id=new_id
                # Standalone composition contains only the selected PCG representation.
                new.model_variant=0
                if mesh.tex_index!=0xffffffff:
                    if mesh.tex_index not in texture_map:
                        texture_map[mesh.tex_index]=len(target.textures)
                        target.textures.append(lg.textures[mesh.tex_index])
                    new.tex_index=texture_map[mesh.tex_index]
                target.meshes.append(new)
            added.append(dict(**placement,output_batch=record['output_batch'],output_group=record['output_group'],
                              output_object=new_id,mesh_count=len(refined),representation='pcg',
                              translation_applied=[0,0,0],ground_alignment='preserved source world coordinates'))
    if not added:raise ValueError('Library contains no validated PCG buildings')
    for path,expected in hashes.items():
        if digest(path)!=expected:raise ValueError('An input changed during composition')
    destination.mkdir(parents=True)
    mtscene.save(destination/'terrain_pcg.mtscene',result)
    report=dict(version=1,scene='terrain_pcg.mtscene',terrain_manifest=str(manifest_path),
                input_sha256=hashes,pcg_library=str(library_path),buildings=added,
                pending_pcg=pending,captured_fallbacks=0,
                coordinates='Original world coordinates preserved, Z-up metres. No terrain or building translation applied.')
    (destination/'manifest.json').write_text(json.dumps(report,indent=2))
    print(f'Added {len(added)} PCG buildings; {len(pending)} await PCG generation; no captured fallbacks.',flush=True)
    return report


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('terrain_manifest');parser.add_argument('pcg_library');parser.add_argument('output_directory')
    args=parser.parse_args()
    compose(args.terrain_manifest,args.pcg_library,args.output_directory)
