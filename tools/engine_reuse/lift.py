"""Strict reusable static x86-to-C lowering around the original instruction graph.

No per-function CPU reset, flag-history heuristic, guessed game calls or no-op
fallback. Every unresolved operation is reported before compilation.
"""
from capstone.x86 import X86_OP_REG, X86_OP_IMM, X86_OP_MEM, X86_REG_FS, X86_REG_GS
from .upstream import Lifter
from .flags import FlagsMixin
from .fpu import FPUMixin
from .extra import ExtraMixin


class EngineLifter(ExtraMixin, FlagsMixin, FPUMixin, Lifter):
    def __init__(self, *args, trap_unsupported=False, static_functions=True, **kwargs):
        super().__init__(*args, **kwargs)
        self.function = None
        self.trap_unsupported = trap_unsupported
        self.static_functions = static_functions
        self.unsupported = []

    def _fmt_mem_addr(self, memory):
        if memory.segment in (X86_REG_FS, X86_REG_GS):
            return f'(ENGINE_FS_BASE + {super()._fmt_mem_addr(memory)})'
        return super()._fmt_mem_addr(memory)

    def _fmt_mem_read(self, memory, size):
        if size not in (1,2,4,8):
            raise ValueError(f'Unsupported memory load size {size}')
        return f'engine_read_u{size*8}(cpu, {self._fmt_mem_addr(memory)})'

    def _fmt_mem_write(self, memory, size, value):
        if size not in (1,2,4,8):
            raise ValueError(f'Unsupported memory store size {size}')
        return f'engine_write_u{size*8}(cpu, {self._fmt_mem_addr(memory)}, {value})'

    def _check_flag_clobber(self, destination, lines):
        pass # EFLAGS is real shared state, not a deferred operand expression.

    def _branch(self, target):
        if target in self.function.blocks:
            return f'goto L_{target:08X};'
        return f'engine_dispatch(cpu, 0x{target:08X}u); return;'

    def lift_instruction(self, insn):
        m, ops = insn.mnemonic, insn.operands or []
        if m in ('sahf','lahf','stc','clc','cmc'):
            return {'sahf':['eflags = (eflags & ~0xd5u) | (HI8(eax) & 0xd5u);'],
                    'lahf':['SET_HI8(eax, (eflags & 0xd5u) | 2u);'],
                    'stc':['eflags |= 1u;'],'clc':['eflags &= ~1u;'],
                    'cmc':['eflags ^= 1u;']}[m]
        if m in ('cld','std'):
            return ['eflags &= ~0x400u;' if m == 'cld' else 'eflags |= 0x400u;']
        if m in ('div','idiv'):
            if len(ops)!=1 or ops[0].size not in (1,2,4):
                raise ValueError('Unsupported DIV encoding')
            return [f'engine_divide(cpu, {self._fmt_read(ops[0])}, {ops[0].size*8}, {int(m=="idiv")});']
        generated = self.lift_extra_instruction(insn)
        if generated is not None:
            return generated
        generated = self.lift_flag_instruction(insn)
        if generated is not None:
            return generated
        generated = self.lift_fpu_instruction(insn)
        if generated is not None:
            return generated
        if m == 'call':
            if len(ops)!=1 or ops[0].size!=4:
                raise ValueError('Unsupported CALL operand')
            return [f'{{ uint32_t target = {self._fmt_read(ops[0])};',
                    f'engine_push(cpu, 0x{insn.end_address:08X}u, 4);',
                    'engine_dispatch(cpu, target);',
                    f'if (cpu->pc != 0x{insn.end_address:08X}u) {{ if (cpu->pc >= 0x{self._flo:08X}u && cpu->pc < 0x{self._fhi:08X}u) goto L_ENTRY; else return; }} }}']
        if m == 'jmp':
            target=insn.get_branch_target()
            if target is not None:
                return [self._branch(target)]
            if not ops or ops[0].size!=4:
                raise ValueError('Unsupported indirect jump')
            lines=[f'switch ({self._fmt_read(ops[0])}) {{']
            lines += [f'case 0x{a:08X}u: goto L_{a:08X};' for a in sorted(self.function.blocks)]
            lines += [f'default: engine_dispatch(cpu, {self._fmt_read(ops[0])}); return; }}']
            return lines
        if m in ('jecxz','jcxz'):
            reg='ecx' if m=='jecxz' else 'LO16(ecx)'
            return [f'if ({reg} == 0) {{ {self._branch(insn.get_branch_target())} }}']
        if insn.is_cond_jump:
            return [f'if ({self.flags_condition(m)}) {{ {self._branch(insn.get_branch_target())} }}']
        if m.startswith('set'):
            return [self._fmt_write(ops[0],f'({self.flags_condition(m)} ? 1u : 0u)')+';']
        if m.startswith('cmov'):
            # Read memory even when the condition is false, as the source does.
            return [f'{{ uint32_t value = {self._fmt_read(ops[1])};',
                    f'if ({self.flags_condition(m)}) {{ {self._fmt_write(ops[0],"value")}; }} }}']
        if m in ('ret','retn'):
            if 0x66 in insn.bytes[:1]:
                raise ValueError('16-bit return unsupported')
            n=ops[0].imm if ops else 0
            return ['cpu->pc = engine_pop(cpu, 4);',f'esp += {n}u;', 'return;']
        if m=='push':
            if len(ops)!=1 or ops[0].size not in (2,4):
                raise ValueError('Unsupported PUSH')
            return [f'engine_push(cpu, {self._fmt_read(ops[0])}, {ops[0].size});']
        if m=='pop':
            if len(ops)!=1 or ops[0].size not in (2,4):
                raise ValueError('Unsupported POP')
            return [f'{{ uint32_t value=engine_pop(cpu,{ops[0].size}); {self._fmt_write(ops[0],"value")}; }}']
        if m=='leave':
            return ['esp=ebp;ebp=engine_pop(cpu,4);']
        if m in ('pushfd','popfd'):
            return ['engine_push(cpu, eflags, 4);'] if m=='pushfd' else ['eflags = engine_pop(cpu,4);']
        if m=='not':
            return [self._fmt_write(ops[0],f'~({self._fmt_read(ops[0])})')+';']
        if m in ('rep movsb','rep movsw','rep movsd'):
            if 0x67 in insn.bytes[:2]:
                raise ValueError('16-bit string addressing unsupported')
            size={'rep movsb':1,'rep movsw':2,'rep movsd':4}[m]
            return [f'while(ecx) {{ uint32_t value=engine_read_u{size*8}(cpu,esi); engine_write_u{size*8}(cpu,edi,value); uint32_t step=(eflags&0x400u)?(uint32_t)-{size}:{size};esi+=step;edi+=step;ecx--; }}']
        if m in ('rep stosb','rep stosw','rep stosd'):
            if 0x67 in insn.bytes[:2]:
                raise ValueError('16-bit string addressing unsupported')
            size={'rep stosb':1,'rep stosw':2,'rep stosd':4}[m]
            return [f'while(ecx) {{ engine_write_u{size*8}(cpu,edi,eax); edi+=(eflags&0x400u)?(uint32_t)-{size}:{size}; ecx--; }}']
        # Keep only already-inspected flag-neutral upstream instructions. All
        # other upstream branches include heuristic or partial approximations.
        if m in ('mov','movzx','movsx','lea','nop','cdq','cwde','cbw','bswap','cwd'):
            result=super().lift_instruction(insn)
            if any(s in '\n'.join(result) for s in ('unknown reg','seg reg','???')):
                raise ValueError('Unsupported register operand')
            return result
        raise NotImplementedError(f'Unsupported instruction {insn.address:08X}: {m} {insn.op_str}')

    def lift_function(self, function):
        self.function=function
        storage='static void' if self.static_functions else 'void'
        self._flo=function.address
        self._fhi=max(b.end for b in function.blocks.values())
        # Entry prologue (L_ENTRY): dispatch cpu->pc to any block leader. Used both on first
        # entry and to resume locally after a non-local return (setjmp/longjmp, SEH unwind).
        leaders=sorted(function.blocks)
        lines=[f'{storage} {function.name}(EngineCPU *cpu) {{','L_ENTRY: switch (cpu->pc) {']
        lines+= [f'case 0x{a:08X}u: goto L_{a:08X};' for a in leaders]
        lines+= ['default: engine_fail(cpu, "dispatch to non-leader address"); }']
        for address,block in sorted(function.blocks.items()):
            lines.append(f'L_{address:08X}:;')
            for insn in block.instructions:
                lines.append(f'engine_step(cpu,0x{insn.address:08X}u); /* {insn.mnemonic} {insn.op_str} */')
                try:
                    lines.extend(self.lift_instruction(insn))
                except (ValueError,NotImplementedError) as error:
                    if not self.trap_unsupported:
                        raise
                    self.unsupported.append(dict(address=f'{insn.address:08X}',reason=str(error)))
                    lines.append('engine_fail(cpu, "unsupported original instruction");')
            last=block.instructions[-1]
            if not last.is_ret and not last.is_uncond_jump:
                lines.append(self._branch(last.end_address))
        return '\n'.join(lines+['}'])+'\n'
