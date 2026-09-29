#!/usr/bin/env python3
"""Merge reviewed PNG textures into an existing HVTEX001 pack.

Pixels are only decoded to native BGRA8. Masked images require the explicit
source-nearest alpha policy and exact verification. Replacing an existing
entry requires its reviewed pixel digest. Original packs/images stay unchanged.
"""
import argparse
import hashlib
import json
import mmap
from pathlib import Path
import struct
from PIL import Image

HEADER = struct.Struct('<8sIIQQ')
ENTRY = struct.Struct('<IIIIQQ')

def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(1024*1024), b''): h.update(block)
    return h.hexdigest()

def entries(data):
    if len(data) < 32: raise ValueError('Truncated header')
    magic, count, stride, start, size = HEADER.unpack_from(data)
    if (magic, stride, start, size) != (b'HVTEX001',32,32,len(data)) or not 0 < count <= 65536:
        raise ValueError('Invalid header')
    end, previous, result = 32+count*32, -1, {}
    if end > size: raise ValueError('Truncated table')
    for i in range(count):
        crc,w,h,flags,offset,n = ENTRY.unpack_from(data,32+i*32)
        if not (crc > previous and 0<w<=8192 and 0<h<=8192 and flags==1 and n==w*h*4 and end<=offset<=size-n):
            raise ValueError('Invalid entry')
        result[crc]=(w,h,offset,n); end=offset+n; previous=crc
    return result

def merge(base, manifest, output):
    if output.exists() or output.with_suffix('.json').exists(): raise FileExistsError(output)
    source_hash = sha(base)
    additions, notes, replacement_digests = {}, [], {}
    for row in json.loads(manifest.read_text()):
        if row['status'] != 'accepted': continue
        crc = int(row['hash'],16)
        if not 0 <= crc <= 0xffffffff or crc in additions: raise ValueError('Duplicate/invalid hash')
        original, generated = Path(row['original']), Path(row['generated'])
        if sha(original)!=row['originalPNG_SHA256'] or sha(generated)!=row['generatedPNG_SHA256']:
            raise ValueError('Image changed after review')
        with Image.open(original) as src, Image.open(generated) as img:
            rgba=img.convert('RGBA'); w,h=rgba.size
            source_alpha=src.convert('RGBA').getchannel('A')
            if not (src.width<=w<=2048 and src.height<=h<=2048):
                raise ValueError('Invalid HD dimensions')
            if w*src.height != h*src.width: raise ValueError('Aspect ratio changed')
            if source_alpha.getextrema() != (255,255):
                if row.get('alphaPolicy') != 'source-nearest':
                    raise ValueError('Nonopaque source requires separate alpha review')
                expected=source_alpha.resize((w,h),Image.Resampling.NEAREST)
                if rgba.getchannel('A').tobytes()!=expected.tobytes():
                    raise ValueError('Source alpha mask changed')
            elif rgba.getchannel('A').getextrema()!=(255,255):
                raise ValueError('Unexpected transparency')
            data=rgba.tobytes('raw','BGRA')
        additions[crc]=(w,h,data)
        if 'replacesPixelSHA256' in row: replacement_digests[crc]=row['replacesPixelSHA256']
        notes.append({**row,'width':w,'height':h,'bytes':len(data),'pixelSHA256':hashlib.sha256(data).hexdigest()})
    if not additions: raise ValueError('No accepted images')
    output.parent.mkdir(parents=True,exist_ok=True)
    temporary=output.with_suffix(output.suffix+'.partial')
    with base.open('rb') as f, mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ) as data:
        old=entries(data)
        replacements=old.keys() & additions.keys()
        if replacements!=replacement_digests.keys(): raise ValueError('Existing replacements require exact reviewed pixel digests')
        for crc in replacements:
            _,_,offset,n=old[crc]
            if hashlib.sha256(data[offset:offset+n]).hexdigest()!=replacement_digests[crc]:
                raise ValueError('Existing replacement changed after review')
        hashes=sorted(old.keys() | additions.keys())
        if len(hashes)>65536: raise ValueError('Too many entries')
        table=[]
        with temporary.open('x+b') as out:
            out.write(bytes(32+32*len(hashes)))
            for crc in hashes:
                if crc in additions: w,h,pixels=additions[crc]
                else:
                    w,h,offset,n=old[crc]; pixels=data[offset:offset+n]
                table.append((crc,w,h,1,out.tell(),len(pixels)))
                out.write(pixels)
            size=out.tell();out.seek(0)
            out.write(HEADER.pack(b'HVTEX001',len(table),32,32,size))
            for row in table:out.write(ENTRY.pack(*row))
        with temporary.open('rb') as f2, mmap.mmap(f2.fileno(),0,access=mmap.ACCESS_READ) as check:
            new=entries(check)
            for crc,(w,h,offset,n) in old.items():
                if crc in replacements: continue
                nw,nh,no,nn=new[crc]
                if (w,h,n)!=(nw,nh,nn) or data[offset:offset+n]!=check[no:no+nn]:
                    raise ValueError('Existing entry changed')
            for crc,(w,h,pixels) in additions.items():
                nw,nh,no,nn=new[crc]
                if (w,h,len(pixels))!=(nw,nh,nn) or pixels!=check[no:no+nn]:
                    raise ValueError('New pixels changed')
    if sha(base)!=source_hash: raise ValueError('Source pack changed')
    temporary.rename(output)
    receipt=dict(baseSHA256=source_hash,manifestSHA256=sha(manifest),packSHA256=sha(output),
                 originalEntries=len(old),addedEntries=len(additions)-len(replacements),replacedEntries=len(replacements),count=len(table),bytes=size,
                 preservedAllExistingPixels=not bool(replacements),preservedAllUnselectedPixels=True,
                 resampled=False,alphaReconstructed=False,textures=notes)
    output.with_suffix('.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps({k:v for k,v in receipt.items() if k!='textures'},indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('base','manifest','output'):p.add_argument(name,type=Path)
    a=p.parse_args();merge(a.base,a.manifest,a.output)
