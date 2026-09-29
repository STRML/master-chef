"""Pinned MIT-licensed decoding/lifting tools; never mutate the proven prototype."""
from pathlib import Path
import sys
import os
import importlib
import types

VISION = Path(__file__).resolve().parents[2]
UPSTREAM = VISION / 'third_party/xwa'
PACKAGE = '_halo_pinned_xwa_tools'
if PACKAGE not in sys.modules:
    package = types.ModuleType(PACKAGE)
    package.__path__ = [str(UPSTREAM / 'tools')]
    sys.modules[PACKAGE] = package
_disasm = importlib.import_module(PACKAGE + '.disasm')
Disassembler, Instruction = _disasm.Disassembler, _disasm.Instruction
Function, BasicBlock = _disasm.Function, _disasm.BasicBlock
Lifter = importlib.import_module(PACKAGE + '.lifter').Lifter
load_pe = importlib.import_module(PACKAGE + '.translator').load_pe

PE = Path(os.environ.get('HALO_EXE', str(VISION / 'game/halo.exe'))).expanduser().resolve()
PE_SHA256 = 'c9acf0c469543283cfed6d7dc04ade976dbdfc7cb4532cf070386de169c19545'
