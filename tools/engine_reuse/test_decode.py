"""Original instruction regression for conditional fallthrough into shared epilogues."""
import unittest
from .upstream import PE,load_pe
from .decode import EngineDisassembler
from .lift import EngineLifter

class ConditionalFallthroughTests(unittest.TestCase):
 def test_original_pool_reset_foreign_epilogue(self):
  info,data,iat=load_pe(str(PE));decoder=EngineDisassembler(data,info.image_base,info.sections)
  fn=decoder.disassemble_function(0x4d0580,iat,known_functions={0x4d0580,0x4d05ca,0x4d05d0})
  self.assertIn(0x4d05ca,fn.tail_calls)
  self.assertNotIn(0x4d05ca,fn.blocks)
  self.assertIn('engine_dispatch(cpu, 0x004D05CAu); return;',EngineLifter(iat_map=iat).lift_function(fn))
 def test_same_bytes_without_split_keep_internal_epilogue(self):
  info,data,iat=load_pe(str(PE));decoder=EngineDisassembler(data,info.image_base,info.sections)
  fn=decoder.disassemble_function(0x4d0580,iat,known_functions={0x4d0580,0x4d05d0})
  self.assertIn(0x4d05ca,fn.blocks)
  self.assertNotIn(0x4d05ca,fn.tail_calls)

if __name__=='__main__':unittest.main()
