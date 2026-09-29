#!/usr/bin/env python3
"""Small synthetic checks for lossless merging and rejection of unsafe inputs."""
import contextlib
import io
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from PIL import Image
from merge_hd_texture_pack import HEADER, ENTRY, entries, merge, sha

class MergeChecks(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name);self.base=self.root/'base.hvt'
        self.base.write_bytes(HEADER.pack(b'HVTEX001',1,32,32,80)+ENTRY.pack(100,2,2,1,64,16)+bytes(range(16)))
        self.original=self.root/'source.png';self.generated=self.root/'hd.png'
        Image.new('RGBA',(2,2),(10,20,30,255)).save(self.original)
        Image.new('RGBA',(4,4),(40,50,60,255)).save(self.generated)
        self.manifest=self.root/'review.json';self.output=self.root/'merged.hvt'
        self.row=dict(hash='00000032',status='accepted',original=str(self.original),generated=str(self.generated))
        self.save_manifest()
    def save_manifest(self):
        self.row.update(originalPNG_SHA256=sha(self.original),generatedPNG_SHA256=sha(self.generated))
        self.manifest.write_text(json.dumps([self.row]))
    def run_merge(self):
        with contextlib.redirect_stdout(io.StringIO()):merge(self.base,self.manifest,self.output)
    def test_exact_old_and_new_pixels(self):
        before=self.base.read_bytes();self.run_merge();data=self.output.read_bytes();table=entries(data)
        self.assertEqual(list(table),[50,100]);self.assertEqual(self.base.read_bytes(),before)
        w,h,o,n=table[100];self.assertEqual(data[o:o+n],bytes(range(16)))
        w,h,o,n=table[50];self.assertEqual((w,h,n),(4,4,64));self.assertEqual(data[o:o+n],bytes([60,50,40,255])*16)
    def test_duplicate_replacement_rejected(self):
        self.row['hash']='00000064';self.save_manifest()
        with self.assertRaises(ValueError):self.run_merge()
        self.assertFalse(self.output.exists())
    def test_source_alpha_rejected(self):
        Image.new('RGBA',(2,2),(10,20,30,0)).save(self.original);self.save_manifest()
        with self.assertRaises(ValueError):self.run_merge()
    def test_generated_alpha_rejected(self):
        Image.new('RGBA',(4,4),(40,50,60,127)).save(self.generated);self.save_manifest()
        with self.assertRaises(ValueError):self.run_merge()
    def test_reviewed_alpha_exact(self):
        src=Image.open(self.original).convert('RGBA');src.putalpha(Image.frombytes('L',(2,2),bytes([0,64,128,255])));src.save(self.original)
        img=Image.open(self.generated).convert('RGBA');img.putalpha(src.getchannel('A').resize((4,4),Image.Resampling.NEAREST));img.save(self.generated)
        self.row['alphaPolicy']='source-nearest';self.save_manifest();self.run_merge()
        data=self.output.read_bytes();w,h,o,n=entries(data)[50]
        self.assertEqual(data[o+3:o+n:4],img.getchannel('A').tobytes())
    def test_reviewed_alpha_mismatch_rejected(self):
        Image.new('RGBA',(2,2),(10,20,30,0)).save(self.original)
        self.row['alphaPolicy']='source-nearest';self.save_manifest()
        with self.assertRaises(ValueError):self.run_merge()
    def test_digest_bound_replacement(self):
        self.row.update(hash='00000064',replacesPixelSHA256=hashlib.sha256(bytes(range(16))).hexdigest());self.save_manifest();self.run_merge()
        table=entries(self.output.read_bytes());self.assertEqual(list(table),[100]);self.assertEqual(table[100][:2],(4,4))
        receipt=json.loads(self.output.with_suffix('.json').read_text());self.assertEqual(receipt['replacedEntries'],1);self.assertFalse(receipt['preservedAllExistingPixels'])
    def test_wrong_replacement_digest_rejected(self):
        self.row.update(hash='00000064',replacesPixelSHA256='0'*64);self.save_manifest()
        with self.assertRaises(ValueError):self.run_merge()
    def test_replacement_digest_for_absent_entry_rejected(self):
        self.row['replacesPixelSHA256']='0'*64;self.save_manifest()
        with self.assertRaises(ValueError):self.run_merge()
    def test_image_change_after_review_rejected(self):
        Image.new('RGBA',(4,4),(9,8,7,255)).save(self.generated)
        with self.assertRaises(ValueError):self.run_merge()
    def test_aspect_change_rejected(self):
        Image.new('RGBA',(4,5),(40,50,60,255)).save(self.generated);self.save_manifest()
        with self.assertRaises(ValueError):self.run_merge()
    def test_truncated_and_overlapping_ranges_rejected(self):
        original=self.base.read_bytes()
        with self.assertRaises(ValueError):entries(original[:-1])
        bad=original[:32]+ENTRY.pack(100,2,2,1,32,16)+original[64:]
        with self.assertRaises(ValueError):entries(bad)
    def test_held_candidate_not_promoted(self):
        self.row['status']='held';self.save_manifest()
        with self.assertRaises(ValueError):self.run_merge()

if __name__=='__main__':unittest.main()
