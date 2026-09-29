#!/usr/bin/env python3
"""Generate a bounded original-engine closure and report every unresolved boundary."""
import argparse
from collections import Counter, deque
import hashlib
import json
import re
import struct
import time
from engine_reuse.upstream import PE, PE_SHA256, VISION, load_pe
from engine_reuse.decode import EngineDisassembler
from engine_reuse.lift import EngineLifter
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG


def switch_case_count(decoder, by_end, jump, index_reg):
    """Jump-table entry count proven by the guard that precedes an indirect jump.

    Recognizes the MSVC shapes `cmp idx, N ; ja default ; jmp [idx*4+T]` (N+1 entries)
    and `cmp sel, N ; ja default ; movzx idx, byte ptr [sel+B] ; jmp [idx*4+T]`
    (max(B[0..N])+1 entries), walking linear predecessors. Returns 0 when no guard is
    proven, so the caller falls back to the plausibility scan."""
    reg = index_reg; remap = None; insn = jump
    for _ in range(16):
        insn = by_end.get(insn.address)
        if insn is None or insn.is_call or insn.is_ret or (insn.is_jump and not insn.is_cond_jump):
            return 0
        ops = insn.operands or []
        if (remap is None and insn.mnemonic == 'movzx' and len(ops) == 2 and ops[0].type == X86_OP_REG and ops[0].reg == reg
                and ops[1].type == X86_OP_MEM and ops[1].size == 1 and ops[1].mem.base and ops[1].mem.index == 0 and ops[1].mem.disp > 0):
            remap = ops[1].mem.disp & 0xFFFFFFFF; reg = ops[1].mem.base; continue
        if (insn.mnemonic == 'cmp' and len(ops) == 2 and ops[0].type == X86_OP_REG and ops[0].reg == reg
                and ops[1].type == X86_OP_IMM and 0 <= ops[1].imm < 4096):
            n = int(ops[1].imm) + 1
            if remap is None: return n
            selectors = decoder.read_bytes(remap, n)
            return (max(selectors) + 1) if selectors and len(selectors) == n else 0
        # A write to the index register between the compare and the jump hides the guard.
        if ops and ops[0].type == X86_OP_REG and ops[0].reg == reg and insn.mnemonic not in ('cmp', 'test'):
            return 0
    return 0


def direct_calls(address,code,entries):
    """Call fixed function entries directly (ENGINE_DIRECT, engine_hooks.h).

    engine_dispatch's lookup was about a tenth of the engine thread on b30;
    most call sites name a fixed entry, so skip it for those. Hooked entries
    are still routed through engine_dispatch by ENGINE_DIRECT. Each function
    also checks for its own entry before the block switch, which is where
    nearly every call lands."""
    def call(m):
        target=int(m.group(1),16)
        if target not in entries:return m.group(0)
        return f'{{ {m.group(2)}ENGINE_DIRECT(cpu, 0x{target:08X}u, sub_{target:08X});'
    code=re.sub(r'\{ uint32_t target = 0x([0-9A-F]{8})u;\n(engine_push\(cpu, 0x[0-9A-F]{8}u, 4\);\n)engine_dispatch\(cpu, target\);',call,code)
    def jump(m):
        target=int(m.group(1),16)
        if target not in entries:return m.group(0)
        return f'ENGINE_DIRECT(cpu, 0x{target:08X}u, sub_{target:08X}); return;'
    code=re.sub(r'engine_dispatch\(cpu, 0x([0-9A-F]{8})u\); return;',jump,code)
    head=f'void sub_{address:08X}(EngineCPU *cpu) {{\nL_ENTRY: switch (cpu->pc) {{\n'
    if code.startswith(head) and f'case 0x{address:08X}u: goto L_{address:08X};' in code:
        code=(f'void sub_{address:08X}(EngineCPU *cpu) {{\nif (cpu->pc == 0x{address:08X}u) goto L_{address:08X};\n'
              f'L_ENTRY: switch (cpu->pc) {{\n')+code[len(head):]
    return code

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('entries',nargs='*',default=['5590a0','55cfd0','55efd0'])
    parser.add_argument('--max-functions',type=int,default=250)
    parser.add_argument('--label',default='player-movement')
    parser.add_argument('--trap-unsupported',action='store_true',help='Keep known instructions, with explicit runtime failure at every unsupported instruction.')
    parser.add_argument('--chunks',type=int,default=1,help='Split generated functions across N translation units (functions become non-static).')
    parser.add_argument('--discover',action='store_true',help='Also translate code addresses that appear as immediate operands (inline callbacks), iterating to a fixed point.')
    args=parser.parse_args()
    assert 0<args.max_functions<=20000
    assert args.label.replace('-','').isalnum()
    assert hashlib.sha256(PE.read_bytes()).hexdigest()==PE_SHA256
    info,data,iat=load_pe(str(PE));decoder=EngineDisassembler(data,info.image_base,info.sections)
    index=VISION/'decompilation/c9acf0c46954/function-addresses.txt'
    known={int(p.stem,16) for p in (VISION/'decompilation/c9acf0c46954/functions').glob('*.c')} or {int(line,16) for line in index.read_text().split()}
    entries=[]
    for e in args.entries:
        if e.startswith('@'):entries.extend(int(x,16) for x in (VISION/e[1:]).read_text().split())
        else:entries.append(int(e,16))
    known.update(entries)
    pending=deque(entries);seen=set();records=[];generated={};started=time.monotonic()
    out=VISION/'native/build/engine-reuse'/args.label;out.mkdir(parents=True,exist_ok=True)
    discovered=0
    while True:
      while pending and len(seen)<args.max_functions:
          address=pending.popleft()
          if address in seen or not decoder.is_code_address(address):continue
          seen.add(address);extra=()
          if address==0x5590a0:
              # Original guard 559B62 bounds EAX to 0..7; the following byte table
              # maps that selector to 0..2 before the indirect dword jump table.
              selectors=decoder.read_bytes(0x559e30,8)
              assert selectors==bytes.fromhex('0002000001010101'),selectors.hex()
              assert max(selectors)==2
              extra=struct.unpack('<III',decoder.read_bytes(0x559e24,12))
          elif address==0x5061c0:
              # 50628F compares the unsigned selector with 8 before this byte
              # remap; all three original destinations belong to this function.
              assert decoder.read_bytes(0x506434,9)==bytes.fromhex('000102020202010101')
              extra=struct.unpack('<III',decoder.read_bytes(0x506428,12))
          elif address==0x5067b0:
              # Original contact-count dispatch has four code pointers followed
              # by INT3 padding. Runtime still rejects every other destination.
              table=decoder.read_bytes(0x506f9c,20)
              assert table==bytes.fromhex('996c5000b66c5000116d5000256d5000cccccccc')
              extra=struct.unpack('<IIII',table[:16])
          try:
              function=decoder.disassemble_function(address,iat,extra_entries=extra,known_functions=known)
          except Exception as error:
              records.append(dict(entry=f'{address:08X}',undecodable=str(error)));continue
          # Follow switch jump tables: jmp dword ptr [reg*4 + table]. Read absolute code
          # pointers until one leaves the function's plausible range, then re-decode with
          # those targets as extra block leaders. Nested switches converge in a few rounds.
          harvested=set(extra);tables=[]
          for _round in range(4):
              new_targets=set()
              by_end={i.end_address:i for b in function.blocks.values() for i in b.instructions}
              for edge_address,kind,_ in function.indirect_edges:
                  if kind!='jump':continue
                  insn=decoder.instruction_at(edge_address);op=insn.operands[0] if insn.operands else None
                  if op is None or op.type!=X86_OP_MEM or op.mem.scale!=4 or op.mem.index==0 or op.mem.base!=0 or op.mem.disp<=0:continue
                  table=op.mem.disp&0xFFFFFFFF
                  count=switch_case_count(decoder,by_end,insn,op.mem.index)
                  # Alternate entries into this body are listed as functions too (the
                  # script interpreter 424C20 has 424C4E), so bound the targets by the
                  # decoded extent rather than by the next listed entry.
                  import bisect
                  ks=sorted(known);ni=bisect.bisect_right(ks,max(address,function.end-1))
                  upper=ks[ni] if ni<len(ks) else address+0x10000
                  for k in range(count if count else 4096):
                      raw=decoder.read_bytes(table+4*k,4)
                      if not raw or len(raw)<4:break
                      v=struct.unpack('<I',raw)[0]
                      plausible=decoder.is_code_address(v) and address<=v<upper
                      if not plausible or (v in known and v!=address):
                          # With a proven count, listed entries are still reached at run
                          # time through the dispatch fallback; without one the first
                          # implausible pointer ends the table.
                          if count:continue
                          break
                      new_targets.add(v)
                  tables.append(f'{table:08X}')
              new_targets-=harvested;new_targets-=set(function.blocks)
              if not new_targets:break
              try:
                  refined=decoder.disassemble_function(address,iat,extra_entries=tuple(harvested|new_targets),known_functions=known)
              except Exception as error:
                  break
              harvested|=new_targets;function=refined
          row=dict(entry=f'{address:08X}',instructions=function.num_instructions,
                   blocks=len(function.blocks),calls=[f'{a:08X}' for a in sorted(function.calls_to)],
                   tailCalls=[f'{a:08X}' for a in sorted(function.tail_calls)],
                   indirectEdges=function.indirect_edges,jumpTables=sorted(set(tables)))
          try:
              lifter=EngineLifter(iat_map=iat,trap_unsupported=args.trap_unsupported,static_functions=args.chunks<=1)
              code=lifter.lift_function(function)
              generated[address]=code
              row['generatedSHA256']=hashlib.sha256(code.encode()).hexdigest()
              if lifter.unsupported:row['instructionTraps']=lifter.unsupported
          except (ValueError,NotImplementedError) as error:
              row['unsupported']=str(error)
          records.append(row)
          pending.extend(sorted((function.calls_to|function.tail_calls)-seen))
      if not args.discover:break
      new=set()
      for insn in list(decoder.instructions.values()):
          for op in insn.operands or []:
              if op.type==X86_OP_IMM:
                  v=op.imm&0xFFFFFFFF
                  if decoder.is_code_address(v) and v not in seen and v not in decoder.instructions:new.add(v)
      new-=set(pending)
      if not new:break
      discovered+=len(new);known.update(new);pending.extend(sorted(new))
    entries=set(generated)
    for address,code in generated.items():
        (out/f'sub_{address:08X}.c').write_text(direct_calls(address,code,entries))
    addresses=sorted(generated)
    # A smaller chunk count must not leave older translation units in the
    # build wildcard, where they would define the same functions twice.
    chunk_names={f'chunk_{i:03d}.c' for i in range(args.chunks)} if args.chunks>1 else set()
    for previous in out.glob('chunk_*.c'):
        if previous.name not in chunk_names:previous.unlink()
    if args.chunks<=1:
        bundle=['#include "engine_cpu.h"','#include "engine_flags.h"',
                'static void engine_dispatch(EngineCPU*,uint32_t);',
                'int engine_dispatch_external(EngineCPU*,uint32_t);','int engine_dispatch_override(EngineCPU*,uint32_t);','void engine_record(uint32_t);',
                '#include "engine_hooks.h"','#include "engine_registers.h"']
        bundle.extend(f'static void sub_{a:08X}(EngineCPU*);' for a in addresses)
        bundle.extend(f'#include "sub_{a:08X}.c"' for a in addresses)
        bundle+=['typedef void (*EngineFn)(EngineCPU*);',
                 'static const uint32_t engine_entries[]={'+','.join(f'0x{a:08X}u' for a in addresses)+'};',
                 'static const EngineFn engine_fns[]={'+','.join(f'sub_{a:08X}' for a in addresses)+'};',
                 f'enum {{ ENGINE_FN_COUNT = {len(addresses)} }};',
                 'static void engine_dispatch(EngineCPU*cpu,uint32_t address){']
    else:
        header=['#ifndef ENGINE_FUNCTIONS_H','#define ENGINE_FUNCTIONS_H','#include "engine_cpu.h"',
                'void engine_dispatch(EngineCPU*,uint32_t);','int engine_dispatch_external(EngineCPU*,uint32_t);','int engine_dispatch_override(EngineCPU*,uint32_t);','void engine_record(uint32_t);','int engine_dispatch_override(EngineCPU*,uint32_t);','void engine_record(uint32_t);']
        header.extend(f'void sub_{a:08X}(EngineCPU*);' for a in addresses)
        header+=['#include "engine_hooks.h"','#endif']
        (out/'engine_functions.h').write_text('\n'.join(header)+'\n')
        per=(len(addresses)+args.chunks-1)//args.chunks
        for n in range(args.chunks):
            part=addresses[n*per:(n+1)*per]
            chunk=['#include "engine_cpu.h"','#include "engine_flags.h"','#include "engine_functions.h"','#include "engine_registers.h"']
            chunk.extend(f'#include "sub_{a:08X}.c"' for a in part)
            (out/f'chunk_{n:03d}.c').write_text('\n'.join(chunk)+'\n')
        bundle=['#include "engine_cpu.h"','#include "engine_functions.h"',
                'typedef void (*EngineFn)(EngineCPU*);',
                'static const uint32_t engine_entries[]={'+','.join(f'0x{a:08X}u' for a in addresses)+'};',
                'static const EngineFn engine_fns[]={'+','.join(f'sub_{a:08X}' for a in addresses)+'};',
                f'enum {{ ENGINE_FN_COUNT = {len(addresses)} }};',
                'void engine_dispatch(EngineCPU*cpu,uint32_t address){']
    bundle+=['if(address>=0xFE000000u){if(engine_dispatch_external(cpu,address))return;cpu->pc=address;engine_fail(cpu,"unresolved import boundary");}',
             'if(engine_dispatch_override(cpu,address))return;',
             'cpu->pc=address;','engine_record(address);',
             'int lo=0,hi=ENGINE_FN_COUNT-1,idx=-1;',
             'while(lo<=hi){int mid=(lo+hi)>>1;if(engine_entries[mid]<=address){idx=mid;lo=mid+1;}else hi=mid-1;}',
             'if(idx>=0){engine_fns[idx](cpu);return;}',
             'if(engine_dispatch_external(cpu,address))return;',
             'engine_fail(cpu,"unresolved original engine boundary");}',
             '__attribute__((weak)) int engine_dispatch_external(EngineCPU*cpu,uint32_t address){(void)cpu;(void)address;return 0;}',
             '__attribute__((weak)) int engine_dispatch_override(EngineCPU*cpu,uint32_t address){(void)cpu;(void)address;return 0;}',
             '__attribute__((weak)) void engine_record(uint32_t address){(void)address;}',
             'void engine_reuse_entry(EngineCPU*cpu,uint32_t entry){engine_dispatch(cpu,entry);}']
    (out/'engine_bundle.c').write_text('\n'.join(bundle)+'\n')
    receipt=dict(sourceExecutableSHA256=PE_SHA256,seconds=time.monotonic()-started,
        functionsVisited=len(seen),functionsGenerated=len(generated),
        generatedInstructions=sum(r['instructions'] for r in records if 'generatedSHA256'in r),
        instructionTraps=sum(len(r.get('instructionTraps',[])) for r in records),
        frontier=[f'{a:08X}' for a in sorted(set(pending)-seen)],functions=records,
        scope='Static generated code, unresolved functions trap explicitly. Not execution or gameplay evidence.')
    (out/'generation.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps({k:v for k,v in receipt.items() if k not in ('functions','frontier')},indent=2))
    print('Unresolved function entries:',len(receipt['frontier']))
    print('Unsupported:',dict(Counter(r['unsupported'].split(':')[0] for r in records if 'unsupported'in r)))
    print('Undecodable entries:',sum(1 for r in records if 'undecodable' in r))
    print('Discovered via immediates:',discovered)


if __name__=='__main__':main()
