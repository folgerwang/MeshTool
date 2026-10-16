"""Validate a generated city without changing capture or scene files."""
import argparse
import hashlib
import json
from pathlib import Path
import mtscene


def fingerprint(mesh):
    h=hashlib.sha256()
    h.update(str((mesh.tex_index,mesh.object_id,mesh.material,mesh.model_variant)).encode())
    for a in (mesh.translation,mesh.vertices,mesh.uvs,mesh.colors):
        if a is not None:h.update(a.tobytes())
    for prim,idx in mesh.draw_calls:h.update(str(prim).encode());h.update(idx.tobytes())
    return h.hexdigest()


def verify(directory,reference):
    directory=Path(directory);manifest=json.loads((directory/'manifest.json').read_text())
    for path,digest in manifest['inputs'].items():
        assert hashlib.sha256(Path(path).read_bytes()).hexdigest()==digest,path
    assert hashlib.sha256((directory/'effective_rules.json').read_bytes()).hexdigest()==manifest['config']['config_sha256']
    source=mtscene.load(reference);result=mtscene.load(directory/'city.mtscene')
    assert len(source)==len(result)
    assert source.route_data==result.route_data
    glass=0;rendered_buildings=0
    for before,after in zip(source,result):
        assert len(before.groups)==len(after.groups)
        for a,b in zip(before.groups,after.groups):
            assert a.objects==b.objects
            assert a.textures==b.textures[:len(a.textures)]
            assert [fingerprint(m) for m in a.meshes if a.objects[m.object_id].cls!=3]==[fingerprint(m) for m in b.meshes if b.objects[m.object_id].cls!=3]
            glass+=sum(m.material==mtscene.MAT_GLASS for m in b.meshes)
            rendered_buildings+=len({m.object_id for m in b.meshes if b.objects[m.object_id].cls==3})
    doors=0
    excluded={b['building'] for b in manifest['buildings'] if b.get('status')=='excluded_nonbuilding_capture'}
    for path in directory.glob('building_*/tiles.json'):
        if path.parent.name in excluded:continue
        catalog=json.loads(path.read_text());base=catalog['building_base_z']
        for item in catalog['instances']:
            if item.get('kind')=='door_candidate':
                assert base-.01<=item['origin'][2]<=base+catalog['door_rule']['max_sill_offset_m']
                doors+=1
    report=dict(source_hashes_verified=True,config_hash_verified=True,
                nonbuilding_geometry_and_textures_unchanged=True,route_unchanged=True,
                buildings=len(manifest['buildings']),rendered_buildings=rendered_buildings,
                excluded_nonbuilding_captures=[b['building'] for b in manifest['buildings'] if b.get('status')=='excluded_nonbuilding_capture'],glass_meshes=glass,base_level_doors=doors,
                roof_sections=sum(r['status']=='captured_height_surface' for b in manifest['buildings'] for r in b.get('roof',[])),
                fallback_buildings=[b['building'] for b in manifest['buildings'] if any(r['status']!='captured_height_surface' for r in b.get('roof',[]))],
                visual_match_certified=False)
    (directory/'verification.json').write_text(json.dumps(report,indent=2))
    print(json.dumps(report,indent=2))


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('directory');p.add_argument('reference');a=p.parse_args();verify(a.directory,a.reference)
