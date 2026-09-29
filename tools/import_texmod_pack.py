#!/usr/bin/env python3
"""Convert an owner-supplied TexMod TPF/ZIP to a native read-only BGRA pack.

Requires Pillow and numpy. No Windows program is executed. Format references:
https://github.com/GregLando113/texmod-loader/blob/master/utils/TexFile.cc
https://github.com/build-wars/texmod/blob/master/uMod_DX9/uMod_TextureFunction.cpp
Only format facts are used; no upstream implementation is incorporated.
"""
from __future__ import annotations
import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
import zipfile
import numpy as np
from PIL import Image

TPF_PASSWORD = bytes.fromhex('73 2A 63 7D 5F 0A A6 BD 7D 65 7E 67 61 2A 7F 7F 74 61 67 5B 60 70 45 74 5C 22 74 5D 6E 6A 73 41 77 6E 46 47 77 49 0C 4B 46 6F')
HEADER = struct.Struct('<8sIIQQ')
ENTRY = struct.Struct('<IIIIQQ')

def open_tpf(data):
    data = bytearray(data)
    full = len(data) // 4
    np.frombuffer(data, dtype='<u4', count=full)[:] ^= 0x3FA43FA4
    for i in range(full * 4, len(data)):
        data[i] ^= 0xA4
    return zipfile.ZipFile(io.BytesIO(data))

def pixels(data):
    data = bytes(data)
    premultiplied = data[:4] == b'DDS ' and data[84:88] in (b'DXT2', b'DXT4')
    if premultiplied:
        # DXT2/4 store the same blocks as DXT3/5 with premultiplied RGB.
        data = data[:84] + {b'DXT2': b'DXT3', b'DXT4': b'DXT5'}[data[84:88]] + data[88:]
    # Pillow treats BI_RGB 32-bit BMP as RGB, dropping TexMod's alpha channel.
    # Preserve meaningful alpha, but BI_RGB files with an entirely zero spare
    # byte have no alpha (e.g. this pack's crewman uniform), so make them opaque.
    if data[:2] == b'BM' and len(data) >= 54:
        offset = struct.unpack_from('<I', data, 10)[0]
        dib, w, h, planes, bits, compression = struct.unpack_from('<IiiHHI', data, 14)
        if dib >= 40 and planes == 1 and bits == 32 and compression == 0:
            if not (0 < w <= 8192 and 0 < abs(h) <= 8192):
                raise ValueError('Invalid BMP dimensions')
            size = w * abs(h) * 4
            if offset < 54 or offset + size > len(data):
                raise ValueError('Truncated BMP')
            image = Image.frombytes('RGBA', (w, abs(h)), data[offset:offset+size], 'raw', 'BGRA', 0, -1 if h > 0 else 1)
            if image.getchannel('A').getextrema() == (0, 0):
                image.putalpha(255)
        else:
            image = Image.open(io.BytesIO(data)).convert('RGBA')
    else:
        image = Image.open(io.BytesIO(data)).convert('RGBA')
    if not (0 < image.width <= 8192 and 0 < image.height <= 8192):
        raise ValueError('Invalid image dimensions')
    if premultiplied:
        channels = np.array(image, dtype=np.uint16)
        alpha = channels[:, :, 3:4]
        channels[:, :, :3] = np.minimum(255, (channels[:, :, :3] * 255 + alpha // 2) // np.maximum(alpha, 1))
        channels[:, :, :3] *= alpha != 0
        image = Image.fromarray(channels.astype(np.uint8))
    return image.width, image.height, image.tobytes('raw', 'BGRA')

def convert(source, output):
    source_bytes = source.read_bytes()
    if source.suffix.lower() == '.tpf':
        tpf = source_bytes
    else:
        with zipfile.ZipFile(io.BytesIO(source_bytes)) as outer:
            names = [n for n in outer.namelist() if n.lower().endswith('.tpf')]
            if len(names) != 1:
                raise ValueError('Expected exactly one TPF')
            tpf = outer.read(names[0])
    with open_tpf(tpf) as archive:
        definitions = archive.read('texmod.def', pwd=TPF_PASSWORD).decode('utf-8-sig').strip('\0\r\n').splitlines()
        textures = {}
        duplicates = []
        for line in definitions:
            crc, name = line.split('|', 1)
            crc = int(crc, 0)
            if not (0 <= crc <= 0xffffffff):
                raise ValueError('Invalid texture hash')
            if crc in textures:
                # One replacement per source. Preserve the first definition's
                # precedence and report every duplicate rather than hiding it.
                duplicates.append(dict(hash=f'{crc:08x}', selected=textures[crc], ignored=name))
                continue
            textures[crc] = name
        output.parent.mkdir(parents=True, exist_ok=True)
        if output.exists():
            raise FileExistsError(output)
        temporary = output.with_suffix(output.suffix + '.partial')
        rows = []
        with temporary.open('w+b') as stream:
            stream.write(bytes(HEADER.size + len(textures) * ENTRY.size))
            for crc, name in sorted(textures.items()):
                data = archive.read(name, pwd=TPF_PASSWORD)  # checks ZIP CRC
                try:
                    w, h, bgra = pixels(data)
                except Exception as error:
                    raise ValueError(f'Cannot decode {name}: {error}') from error
                position = stream.tell()
                stream.write(bgra)
                rows.append(dict(hash=f'{crc:08x}', source=name, width=w, height=h, offset=position,
                                 bytes=len(bgra), sha256=hashlib.sha256(bgra).hexdigest()))
                if len(rows) % 25 == 0:
                    print(f'Converted {len(rows)}/{len(textures)}', flush=True)
            size = stream.tell()
            stream.seek(0)
            stream.write(HEADER.pack(b'HVTEX001', len(rows), ENTRY.size, HEADER.size, size))
            for row in rows:
                stream.write(ENTRY.pack(int(row['hash'], 16), row['width'], row['height'], 1, row['offset'], row['bytes']))
        temporary.rename(output)
    receipt = dict(sourceSHA256=hashlib.sha256(source_bytes).hexdigest(), tpfSHA256=hashlib.sha256(tpf).hexdigest(),
                   packSHA256=hashlib.sha256(output.read_bytes()).hexdigest(), count=len(rows), bytes=size,
                   duplicateDefinitions=duplicates, textures=rows)
    output.with_suffix('.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps({k: v for k, v in receipt.items() if k != 'textures'}), flush=True)

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source', type=Path)
    p.add_argument('output', type=Path)
    a = p.parse_args()
    convert(a.source, a.output)
