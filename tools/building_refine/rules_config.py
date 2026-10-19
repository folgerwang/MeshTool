"""Validated runtime configuration and reproducible effective-rule snapshots."""
import copy
import hashlib
import json
import math
from pathlib import Path

DEFAULT_PATH=Path(__file__).with_name('building_rules.json')
DEFAULTS=json.loads(DEFAULT_PATH.read_text())
RULES=copy.deepcopy(DEFAULTS)
CONFIG_PATH=DEFAULT_PATH


def configure(path=None):
    global CONFIG_PATH
    requested=Path(path).resolve() if path else DEFAULT_PATH.resolve()
    overrides=json.loads(requested.read_text())
    def merge(default,value,location=''):
        if isinstance(default,dict):
            if not isinstance(value,dict):raise ValueError(f'{location}: expected object')
            unknown=set(value)-set(default)
            if unknown:raise ValueError(f'{location}: unknown rules {sorted(unknown)}')
            return {k:merge(v,value[k],location+'.'+k) if k in value else copy.deepcopy(v) for k,v in default.items()}
        if isinstance(default,bool):
            if not isinstance(value,bool):raise ValueError(f'{location}: expected boolean')
        elif isinstance(default,(int,float)):
            if isinstance(value,bool) or not isinstance(value,(int,float)) or not math.isfinite(value) or value<=0:raise ValueError(f'{location}: expected finite positive number')
            if isinstance(default,int) and not isinstance(value,int):raise ValueError(f'{location}: expected integer')
        elif not isinstance(value,type(default)):raise ValueError(f'{location}: wrong value type')
        return value
    result=merge(DEFAULTS,overrides)
    if result['schema_version']!=1:raise ValueError('Unsupported rules schema')
    for key in ('normal_alignment','min_observed_fraction'):
        if result['captured_glazing'][key]>1:raise ValueError('Invalid captured glazing fraction: '+key)
    for section in ('doors','windows'):
        for dimension in ('width','height'):
            if result[section]['min_'+dimension+'_m']>result[section]['max_'+dimension+'_m']:raise ValueError('Invalid size range')
    for section,key in [('doors','min_observed_fraction'),('pattern','match_threshold'),('pattern','min_regular_fraction'),('roofs','min_coverage')]:
        if result[section][key]>1:raise ValueError(f'{section}.{key}: expected fraction')
    for key in ('min_support','edge_fraction','sample_margin_fraction','neighbor_offset_fraction'):
        if result['window_members'][key]>=1:raise ValueError(f'window_members.{key}: expected fraction below one')
    if result['window_members']['min_width_m']>result['window_members']['max_width_m']:raise ValueError('Invalid window member width range')
    if result['pattern']['min_template_contrast_ratio']>1:raise ValueError('Invalid template contrast ratio')
    if result['materials']['frame_sample_percentile']>100:raise ValueError('Invalid frame percentile')
    if result['roofs']['region_joint_m']>=result['roofs']['mesh_resolution_m']*.1:raise ValueError('Roof construction joint too wide')
    if result['materials']['wall_mode']!='captured' or result['roofs']['fallback']!='preserve_shell':raise ValueError('Unsupported material/fallback policy')
    if set(result['materials']['glass_parts'])!={'glazing','glass','roof_glass'}:raise ValueError('Glass assignment must cover window and roof glazing only')
    if not 0<result['windows']['frame_max_fraction']<.5:raise ValueError('Frame must leave an opening')
    if not 0<result['windows']['arch_crown_fraction']<1:raise ValueError('Invalid arch crown fraction')
    color=result['materials']['fallback_wall_rgb']
    if len(color)!=3 or any(not isinstance(v,int) or not 0<=v<=255 for v in color):raise ValueError('Invalid fallback RGB')
    if not all(result['source'].values()) or not all(result['validation'].values()):raise ValueError('Source preservation and validation are mandatory')
    RULES.clear();RULES.update(result);CONFIG_PATH=requested
    return RULES


def snapshot(output):
    payload=json.dumps(RULES,indent=2,sort_keys=True)+'\n'
    path=Path(output)/'effective_rules.json';path.write_bytes(payload.encode())
    return dict(config_path=str(CONFIG_PATH),ruleset=RULES['ruleset'],effective_config=str(path),config_sha256=hashlib.sha256(payload.encode()).hexdigest())


configure()
