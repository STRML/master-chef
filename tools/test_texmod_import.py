#!/usr/bin/env python3
"""Importer regressions: preserve BMP alpha, orientation, DDS premultiplication."""
import io
import struct
import unittest
from PIL import Image
from import_texmod_pack import pixels

class DecodeChecks(unittest.TestCase):
    def test_bmp_alpha_and_bottom_up(self):
        header=bytearray(54)
        header[:2]=b'BM';struct.pack_into('<I',header,2,70);struct.pack_into('<I',header,10,54)
        struct.pack_into('<IiiHHI',header,14,40,2,2,1,32,0)
        bottom=bytes([10,20,30,0,40,50,60,128]);top=bytes([70,80,90,255,100,110,120,17])
        w,h,bgra=pixels(header+bottom+top)
        self.assertEqual((w,h),(2,2));self.assertEqual(bgra,top+bottom)
        with self.assertRaises(ValueError):pixels(header+bottom)
        # BI_RGB's unused zero byte is not an authored transparency channel.
        zero_alpha=bytes([10,20,30,0])*4
        self.assertEqual(pixels(header+zero_alpha)[2],bytes([10,20,30,255])*4)

    def test_png_channels(self):
        image=Image.new('RGBA',(2,1),(10,20,30,40));stream=io.BytesIO();image.save(stream,format='PNG')
        self.assertEqual(pixels(stream.getvalue()),(2,1,bytes([30,20,10,40])*2))

    def test_dxt2_unpremultiply(self):
        header=bytearray(128);header[:4]=b'DDS '
        struct.pack_into('<I',header,4,124);struct.pack_into('<I',header,8,0x81007)
        struct.pack_into('<II',header,12,4,4);struct.pack_into('<I',header,20,16)
        struct.pack_into('<II',header,76,32,4);header[84:88]=b'DXT2';struct.pack_into('<I',header,108,0x1000)
        # Half alpha, half-bright red in RGB565; DXT2 stores premultiplied RGB.
        block=bytes([0x88]*8)+struct.pack('<HHI',0x8000,0,0)
        w,h,bgra=pixels(header+block);self.assertEqual((w,h),(4,4))
        self.assertEqual(bgra[:2],bytes(2));self.assertGreater(bgra[2],240);self.assertEqual(bgra[3],136)

if __name__=='__main__':unittest.main()
