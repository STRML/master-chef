#!/usr/bin/env python3
"""Emit the executable's static import table and TLS directory as C for the host loader."""
import argparse, io, contextlib, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from engine_reuse.upstream import PE, PE_SHA256, VISION, load_pe

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--output-dir',type=Path,default=VISION/'native/build/engine-reuse/whole-exe')
    args=parser.parse_args()
    with contextlib.redirect_stdout(io.StringIO()):
        info, data, iat = load_pe(str(PE))
    e_lfanew = struct.unpack_from('<I', data, 0x3c)[0]; opt = e_lfanew + 24
    entry = info.image_base + struct.unpack_from('<I', data, opt + 16)[0]
    size_of_image = struct.unpack_from('<I', data, opt + 56)[0]
    tls_rva, tls_size = struct.unpack_from('<II', data, opt + 96 + 9 * 8)
    out = [f'/* Generated from halo.exe {PE_SHA256[:12]} by tools/export_engine_imports.py */',
           '#include "host.h"',
           f'const uint32_t engine_pe_entry_point = 0x{entry:08X}u;',
           f'const uint32_t engine_pe_image_base = 0x{info.image_base:08X}u;',
           f'const uint32_t engine_pe_size_of_image = 0x{size_of_image:08X}u;',
           f'const uint32_t engine_pe_tls_directory = 0x{(info.image_base + tls_rva) if tls_size else 0:08X}u;',
           'const EngineSection engine_pe_sections[] = {']
    for s in info.sections:
        out.append(f'    {{"{s.name}", 0x{info.image_base + s.virtual_address:08X}u, 0x{s.virtual_size:X}u, 0x{s.raw_offset:X}u, 0x{s.raw_size:X}u, 0x{s.characteristics:08X}u}},')
    out += ['};', f'const size_t engine_pe_section_count = {len(info.sections)};',
            'const EngineImport engine_imports[] = {']
    for i in sorted(info.imports, key=lambda i: i.iat_rva):
        name = i.name if i.name else f'#{i.ordinal}'
        out.append(f'    {{0x{info.image_base + i.iat_rva:08X}u, "{i.dll}", "{name}"}},')
    out += ['};', f'const size_t engine_import_count = {len(info.imports)};']
    output=args.output_dir.expanduser();output=output if output.is_absolute() else VISION/output
    output.mkdir(parents=True,exist_ok=True)
    target = output / 'engine_imports.c'
    target.write_text('\n'.join(out) + '\n')
    print(f'wrote {target} ({len(info.imports)} imports, entry 0x{entry:08X}, tls dir 0x{info.image_base + tls_rva:08X})')

if __name__ == '__main__':
    main()
