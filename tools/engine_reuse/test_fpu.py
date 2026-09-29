#!/usr/bin/env python3
"""Execute identical x87 snippets in Unicorn and statically generated native C."""
from pathlib import Path
import json
import math
import random
import struct
import subprocess
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import *
from .upstream import VISION, Instruction, Lifter
from .fpu import FPUMixin

DATA = 0x200000
CODE = 0x100000
BUILD = VISION / 'native/build/engine-reuse-fpu'


class FloatLifter(FPUMixin, Lifter):
    def lift_instruction(self, insn):
        code = self.lift_fpu_instruction(insn)
        if code is None:
            raise ValueError(insn.mnemonic)
        return code


def absolute(op, offset):
    return bytes.fromhex(op) + struct.pack('<I', DATA+offset)


def snippets():
    load_a, load_b = absolute('dd05', 0), absolute('dd05', 8)
    store = absolute('dd1d', 32)
    rows = []
    for name, opcode in [('addp','dec1'),('subp','dee9'),('subrp','dee1'),
                         ('mulp','dec9'),('divp','def9'),('divrp','def1')]:
        rows.append((name, load_a+load_b+bytes.fromhex(opcode)+store, 'double'))
    for name, opcode in [('sub_st1','dce9'),('subr_st1','dce1'),('div_st1','dcf9'),
                         ('divr_st1','dcf1'),('add_st1','dcc1')]:
        rows.append((name,load_a+load_b+bytes.fromhex(opcode)+bytes.fromhex('ddd8')+store,'double'))
    rows.append(('subp_st2',load_a+load_b+absolute('dd05',16)+bytes.fromhex('deea ddd8')+store,'double'))
    for name,opcode in [('fxch_st0','d9c8'),('fxch_st1','d9c9')]:
        rows.append((name,load_a+load_b+bytes.fromhex(opcode)+store+bytes.fromhex('ddd8'),'double'))
    rows.append(('fstp_st1',load_a+load_b+bytes.fromhex('ddd9')+store,'double'))
    for name,opcode in [('fcompp','ded9'),('fucompp','dae9')]:
        rows.append((name,load_a+load_b+bytes.fromhex(opcode)+bytes.fromhex('dfe0'),'status'))
    for name,opcode in [('fcompi','dff1'),('fucompi','dfe9')]:
        rows.append((name,load_a+load_b+bytes.fromhex(opcode)+bytes.fromhex('ddd8'),'eflags'))
    for name,opcode in [('fist16','df1d'),('fist32','db1d'),('fist64','df3d'),('fisttp32','db0d')]:
        rows.append((name,load_a+absolute(opcode,32),name))
    rows.append(('f32_store',load_a+absolute('d91d',32),'f32'))
    rows.append(('frndint',load_a+bytes.fromhex('d9fc')+store,'double'))
    return rows


def generate(rows):
    md=Cs(CS_ARCH_X86,CS_MODE_32);md.detail=True
    functions=[]
    for index,(_,code,_) in enumerate(rows):
        body=[]
        for d in md.disasm(code,CODE):
            instruction=Instruction(d.address,d.size,d.mnemonic,d.op_str,bytes(d.bytes),list(d.operands))
            body.extend(FloatLifter().lift_instruction(instruction))
        functions.append(f'static void snippet_{index}(EngineCPU *cpu) {{\n'+ '\n'.join(body)+'\n}')
    source='''#include "engine_cpu.h"
#include <stdio.h>
#include <setjmp.h>
#define eax (cpu->gpr[0])
#define SET_LO16(r,v) ((r)=((r)&0xffff0000u)|(uint16_t)(v))
static uint8_t memory[64];
static jmp_buf failure_jump;
static void failed(EngineCPU*c,const char*r){(void)c;(void)r;longjmp(failure_jump,1);}
static void*address(EngineCPU*c,uint32_t a,size_t n){(void)c;return a>=0x200000 && (uint64_t)a+n<=0x200040 ? memory+(a-0x200000):NULL;}
typedef struct {uint32_t index;uint16_t control;uint16_t reserved;double a,b,c;} Input;
typedef struct {uint8_t result[8];uint32_t ax,flags;uint16_t status;uint16_t error;} Output;
'''+ '\n'.join(functions)+'''\nint main(int argc,char**argv){if(argc!=3)return 64;
FILE*in=fopen(argv[1],"rb"),*out=fopen(argv[2],"wb");if(!in||!out)return 65;
Input input;while(fread(&input,sizeof input,1,in)==1){EngineCPU state={0};EngineCPU*cpu=&state;
memset(memory,0,sizeof memory);memcpy(memory,&input.a,24);
cpu->address=address;cpu->failure=failed;cpu->flags=0x8d7;cpu->gpr[0]=0xabba0000;
engine_fp_init(cpu);engine_fp_control(cpu,input.control);Output result={0};
if(setjmp(failure_jump))result.error=1;else switch(input.index){
'''+''.join(f'case {i}:snippet_{i}(cpu);break;\n' for i in range(len(rows)))+'''
default:return 66;}memcpy(result.result,memory+32,8);result.ax=cpu->gpr[0];result.flags=cpu->flags;
result.status=engine_fp_status(cpu);fwrite(&result,sizeof result,1,out);}
fclose(in);fclose(out);return 0;}\n'''
    (BUILD/'test.c').write_text(source)


def reference(code,control,a,b,c):
    u=Uc(UC_ARCH_X86,UC_MODE_32);u.mem_map(CODE,0x1000);u.mem_map(DATA,0x1000)
    u.mem_write(CODE,code);u.mem_write(DATA,struct.pack('<ddd',a,b,c))
    u.reg_write(UC_X86_REG_EAX,0xabba0000);u.reg_write(UC_X86_REG_EFLAGS,0x8d7)
    u.reg_write(UC_X86_REG_FPCW,control);u.reg_write(UC_X86_REG_FPSW,0);u.reg_write(UC_X86_REG_FPTAG,0xffff)
    u.emu_start(CODE,CODE+len(code),count=100)
    return (bytes(u.mem_read(DATA+32,8)),u.reg_read(UC_X86_REG_EAX),u.reg_read(UC_X86_REG_EFLAGS),u.reg_read(UC_X86_REG_FPSW))


def main():
    BUILD.mkdir(parents=True,exist_ok=True)
    rows=snippets();generate(rows)
    rng=random.Random(773)
    samples=[(5.,3.,2.),(-5.,3.,-2.),(1.5,2.5,3.5),(-1.5,-2.5,-3.5),
             (0.,1.,2.),(-0.,-1.,2.),(32767.5,1.,2.),(-32768.5,1.,2.),
             (2147483647.5,1.,2.),(-2147483648.5,1.,2.),(2**53,3.,1.),
             (2**63,1.,2.),(-float(2**63),1.,2.)]
    samples += [(rng.uniform(-1e6,1e6),rng.uniform(.125,1e6),rng.uniform(-100,100)) for _ in range(32)]
    cases=[];expected=[]
    for i,(name,code,kind) in enumerate(rows):
        for control in (0x027f,0x067f,0x0a7f,0x0e7f):
            for a,b,c in samples:
                if 'divr' in name and a==0:continue
                cases.append((i,control,a,b,c));expected.append(reference(code,control,a,b,c))
        if kind in ('status','eflags'):
            for a,b in [(math.nan,1.),(1.,math.nan),(math.inf,math.inf),(-math.inf,math.inf)]:
                cases.append((i,0x027f,a,b,0.));expected.append(reference(code,0x027f,a,b,0.))
    (BUILD/'input.bin').write_bytes(b''.join(struct.pack('<IH2xddd',*case) for case in cases))
    # Unicorn's FISTTP overflow and FCOMI flag clearing differ from x86. Run
    # these exact instructions through the independent installed Rosetta path.
    (BUILD/'x86-secondary.c').write_text(r'''
#include <stdint.h>
#include <stdio.h>
typedef struct {uint32_t index;uint16_t control,pad;double a,b,c;} Input;
int main(int argc,char**argv){if(argc!=3)return 64;
FILE*in=fopen(argv[1],"rb"),*out=fopen(argv[2],"wb");if(!in||!out)return 65;
Input v;while(fread(&v,sizeof v,1,in)==1){uint32_t converted;uint64_t after,flags=0x8d7;
__asm__ volatile("fldl %[a]; fisttpl %[r]" : [r]"=m"(converted):[a]"m"(v.a):"st");
__asm__ volatile("pushq %[f]; popfq; fldl %[a]; fldl %[b]; fcomip %%st(1),%%st; fstp %%st(0); pushfq; popq %[o]"
 : [o]"=r"(after):[f]"r"(flags),[a]"m"(v.a),[b]"m"(v.b):"st","cc");
uint32_t word=(uint32_t)after;fwrite(&converted,4,1,out);fwrite(&word,4,1,out);}
fclose(in);fclose(out);return 0;}
''')
    secondary=BUILD/'x86-secondary'
    subprocess.run(['clang','-arch','x86_64',str(BUILD/'x86-secondary.c'),'-o',str(secondary)],check=True)
    subprocess.run(['arch','-x86_64',str(secondary),str(BUILD/'input.bin'),str(BUILD/'secondary.bin')],check=True)
    independent=list(struct.iter_unpack('<II',(BUILD/'secondary.bin').read_bytes()))
    assert len(independent)==len(cases)
    results={}
    for label,options in [('debug',['-O0']),('optimized',['-O3']),('sanitizer',['-O1','-fsanitize=address,undefined'])]:
        exe=BUILD/label
        subprocess.run(['clang','-std=c11','-frounding-math','-ffp-contract=off',*options,'-I',str(VISION/'native/EngineReuse'),str(BUILD/'test.c'),'-o',str(exe)],check=True)
        output=BUILD/(label+'.bin');subprocess.run([str(exe),str(BUILD/'input.bin'),str(output)],check=True)
        actual=list(struct.iter_unpack('<8sIIHH',output.read_bytes()))
        assert len(actual)==len(cases)
        for n,(case,want,got) in enumerate(zip(cases,expected,actual)):
            name,_,kind=rows[case[0]]
            assert got[4]==0,(label,n,case,'unexpected runtime rejection')
            if kind=='status':equal=((got[1]^want[1])&0xffff7d00)==0
            elif kind=='eflags':
                # Unicorn preserves stale OF/SF/AF for FCOMI. A separately
                # executed x86_64/Rosetta probe confirms their architectural
                # clearing. Compare UC's defined condition result independently.
                equal=((got[2]^want[2])&0x45)==0 and ((got[2]^independent[n][1])&0x8d5)==0
            elif kind=='fisttp32':equal=got[0]==struct.pack('<II',independent[n][0],0)
            else:equal=got[0]==want[0]
            assert equal,(label,n,name,case,want,got)
        results[label]=len(cases)
        print(f'{label}: {len(cases)} original-x86 comparisons PASS',flush=True)
    receipt=dict(snippets=len(rows),cases=len(cases),results=results,
        secondaryOracle='Installed x86_64 Rosetta execution for FISTTP32 and FCOMI; UC bugs independently reproduced.',
        scope='PC53 finite arithmetic; actual C0/C2/C3 and EFLAGS comparisons; not full x87 exception/C1 fidelity or gameplay.')
    (VISION/'native/build/engine-reuse-fpu-validation.json').write_text(json.dumps(receipt,indent=2)+'\n')


if __name__=='__main__':main()
