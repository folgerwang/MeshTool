"""Four-building overview sheets; full comparisons remain the detailed evidence."""
import argparse,json
from pathlib import Path
from PIL import Image,ImageDraw,ImageFont


def run(candidate,reviews):
    candidate,reviews=Path(candidate).resolve(),Path(reviews).resolve()
    names=[r['building'] for r in json.loads((candidate/'manifest.json').read_text())['buildings']]
    names=[n for n in names if (reviews/n/'render_record.json').exists() and
           Path(json.loads((reviews/n/'render_record.json').read_text())['candidate'])==candidate]
    output=reviews/'contact_sheets';output.mkdir(exist_ok=True)
    font=ImageFont.truetype('C:/Windows/Fonts/arial.ttf',20)
    ready=[]
    for start in range(0,len(names),4):
        batch=names[start:start+4]
        if not all((reviews/n/'render_record.json').exists() and
                   Path(json.loads((reviews/n/'render_record.json').read_text())['candidate'])==candidate for n in batch):continue
        sheet=Image.new('RGB',(1000,1180),(25,27,32));draw=ImageDraw.Draw(sheet)
        for i,name in enumerate(batch):
            im=Image.open(reviews/name/'comparison.jpg').convert('RGB');im.thumbnail((500,555))
            x,y=(i%2)*500,(i//2)*590
            draw.text((x+10,y+5),name+' | capture / candidate',font=font,fill='white')
            sheet.paste(im,(x,y+30))
        path=output/f'batch_{start//4:02d}.jpg';sheet.save(path,quality=94)
        ready.append(dict(batch=start//4,buildings=batch,path=str(path)))
    (output/'index.json').write_text(json.dumps(ready,indent=2));print(len(ready),'batches ready')


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('candidate');p.add_argument('reviews');a=p.parse_args();run(a.candidate,a.reviews)
