"""Local index of rendered comparisons and recorded per-building findings."""
import argparse,html,json
from pathlib import Path


def run(ledger,images,output):
    data=json.loads(Path(ledger).read_text());images=Path(images).resolve();cards=[]
    for entry in sorted(data['buildings'],key=lambda e:e['building']):
        name=entry['building'];picture=images/name/'comparison.jpg'
        history=entry['history'];note=history[-1]['notes'] if history else 'Awaiting visual review.'
        latest=entry.get('latest_candidate',{})
        matching=next((h for h in reversed(history) if h.get('building_content_sha256')==latest.get('building_content_sha256') and h.get('building_content_sha256')),None)
        if matching and matching.get('images'):
            picture=Path(matching['images'][0]['path']);note=matching['notes']
        elif latest.get('stage')=='generated_pending_comparison':
            note='Latest candidate awaits visual review. '+note
        status=entry['status'];tasks=', '.join(entry['tasks'])
        if picture.exists():
            media=f'<a href="{picture.as_uri()}"><img loading="lazy" src="{picture.as_uri()}" alt="{name} capture comparison"></a>'
        else:media='<p>Comparison is not ready.</p>'
        cards.append(f'<article data-search="{html.escape(name+" "+status+" "+tasks)}"><h2>{name}</h2><p class="status">{html.escape(status)}</p>{media}<p>{html.escape(note)}</p><details><summary>Next checks</summary><p>{html.escape(tasks)}</p></details></article>')
    document='''<!doctype html><html><meta charset="utf-8"><title>Building capture review</title>
<style>body{background:#171b22;color:#e6e9ee;font:16px system-ui;margin:24px}header{position:sticky;top:0;background:#171b22;padding:12px;z-index:1}input{font:inherit;padding:10px;width:70%;background:#282f39;color:white;border:1px solid #738299}main{display:grid;grid-template-columns:repeat(auto-fit,minmax(440px,1fr));gap:20px}article{border:1px solid #465365;padding:16px;border-radius:8px}img{width:100%;height:auto}h2{margin:0}.status{color:#e8c57b}details{color:#a9bed7}a{color:#a9bed7}</style>
<header><h1>Per-building capture comparison</h1><p>Original capture at left; candidate at right. Two oblique views and an overhead view. Open an image for detail. Review status is independent of successful generation.</p><input id="search" placeholder="Filter by building number, status or next check"></header><main>'''+''.join(cards)+'''</main><script>document.querySelector('#search').oninput=e=>{let q=e.target.value.toLowerCase();document.querySelectorAll('article').forEach(a=>a.hidden=!a.dataset.search.toLowerCase().includes(q))}</script></html>'''
    Path(output).write_text(document,encoding='utf-8')


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('ledger');p.add_argument('images');p.add_argument('output');a=p.parse_args();run(a.ledger,a.images,a.output)
