"""Integer, string, and system instructions that the strict lifter did not cover.

Everything here is lowered exactly (no heuristics). String instructions honor
the direction flag. CPUID reports a plain CPU with no MMX/SSE/3DNow! so the
game selects its x87 code paths; RDTSC reads the host monotonic clock.
"""
from capstone.x86 import X86_OP_REG, X86_OP_IMM, X86_OP_MEM

_STRING_OPCODES = {0xA4:('movs',1),0xA5:('movs',0),0xAA:('stos',1),0xAB:('stos',0),
                   0xAC:('lods',1),0xAD:('lods',0),0xAE:('scas',1),0xAF:('scas',0),
                   0xA6:('cmps',1),0xA7:('cmps',0)}
_PREFIXES = {0xF0,0xF2,0xF3,0x66,0x67,0x2E,0x36,0x3E,0x26,0x64,0x65}


class ExtraMixin:
    def _string_form(self, insn):
        """Return (kind, width_bytes, rep) for a string instruction, else None."""
        b = insn.bytes; i = 0; rep = None; opsize16 = False
        while i < len(b) and b[i] in _PREFIXES:
            if b[i] == 0xF3: rep = 'repe'
            elif b[i] == 0xF2: rep = 'repne'
            elif b[i] == 0x66: opsize16 = True
            i += 1
        if i >= len(b) or b[i] not in _STRING_OPCODES: return None
        kind, byte = _STRING_OPCODES[b[i]]
        width = 1 if byte else (2 if opsize16 else 4)
        return kind, width, rep

    def _string_lines(self, kind, width, rep, insn):
        bits = width*8; step = f'(uint32_t)((eflags & 0x400u) ? -{width} : {width})'
        acc = {1:'LO8(eax)',2:'LO16(eax)',4:'eax'}[width]
        setacc = {1:'SET_LO8(eax,{v})',2:'SET_LO16(eax,{v})',4:'eax=({v})'}[width]
        body = {
            'movs': [f'engine_write_u{bits}(cpu, edi, engine_read_u{bits}(cpu, esi));', f'esi += {step}; edi += {step};'],
            'stos': [f'engine_write_u{bits}(cpu, edi, {acc});', f'edi += {step};'],
            'lods': [setacc.format(v=f'engine_read_u{bits}(cpu, esi)')+';', f'esi += {step};'],
            'scas': [f'(void)engine_flags_sub(&eflags, {acc}, engine_read_u{bits}(cpu, edi), 0u, {bits}u);', f'edi += {step};'],
            'cmps': [f'(void)engine_flags_sub(&eflags, engine_read_u{bits}(cpu, esi), engine_read_u{bits}(cpu, edi), 0u, {bits}u);', f'esi += {step}; edi += {step};'],
        }[kind]
        if rep is None:
            return body
        if kind in ('scas','cmps'):
            stop = '!(eflags & 0x40u)' if rep == 'repe' else '(eflags & 0x40u)'
            return ['while (ecx) {'] + body + ['ecx--;', f'if ({stop}) break;', '}']
        return ['while (ecx) {'] + body + ['ecx--;', '}']

    def _bit_test(self, insn, m, ops):
        base, off = ops[0], ops[1]
        bits = base.size*8
        offv = self._fmt_read(off)
        lines = []
        if base.type == X86_OP_MEM and off.type == X86_OP_REG:
            # Register bit offsets may address outside the operand: adjust the address.
            addr = self._fmt_mem_addr(base.mem)
            lines.append(f'{{ int32_t bitoff = (int32_t)({offv}); uint32_t bitaddr = ({addr}) + (uint32_t)((bitoff >> {5 if bits==32 else 4 if bits==16 else 3}) * {bits//8}); uint32_t bitn = (uint32_t)bitoff & {bits-1}u;')
            lines.append(f'uint32_t bitv = engine_read_u{bits}(cpu, bitaddr);')
            store = f'engine_write_u{bits}(cpu, bitaddr, bitv);'
        else:
            lines.append(f'{{ uint32_t bitn = ({offv}) & {bits-1}u; uint32_t bitv = {self._fmt_read(base)};')
            store = self._fmt_write(base, 'bitv') + ';'
        lines.append('eflags = (eflags & ~1u) | ((bitv >> bitn) & 1u);')
        if m == 'bts': lines.append('bitv |= (1u << bitn);')
        elif m == 'btr': lines.append('bitv &= ~(1u << bitn);')
        elif m == 'btc': lines.append('bitv ^= (1u << bitn);')
        if m != 'bt': lines.append(store)
        lines.append('}')
        return lines

    def lift_extra_instruction(self, insn):
        m, ops = insn.mnemonic, insn.operands or []
        base = m.split()[-1]
        form = self._string_form(insn)
        if form is not None and base in ('movsb','movsw','movsd','stosb','stosw','stosd','lodsb','lodsw','lodsd','scasb','scasw','scasd','cmpsb','cmpsw','cmpsd'):
            kind, width, rep = form
            if 0x67 in insn.bytes[:3]:
                raise ValueError('16-bit string addressing unsupported')
            return self._string_lines(kind, width, rep, insn)
        if base in ('mul','imul') and len(ops) == 1:
            if ops[0].size not in (1,2,4): raise ValueError('Unsupported MUL width')
            return [f'engine_mul1(cpu, {self._fmt_read(ops[0])}, {ops[0].size*8}u, {int(base=="imul")});']
        if base in ('shld','shrd'):
            if len(ops) != 3 or ops[0].size not in (2,4): raise ValueError('Unsupported SHLD/SHRD form')
            v = f'engine_flags_{base}(&eflags, {self._fmt_read(ops[0])}, {self._fmt_read(ops[1])}, {self._fmt_read(ops[2])}, {ops[0].size*8}u)'
            return [self._fmt_write(ops[0], v)+';']
        if base in ('rol','ror','rcl','rcr'):
            if len(ops) != 2 or ops[0].size not in (1,2,4): raise ValueError('Unsupported rotate form')
            v = f'engine_flags_{base}(&eflags, {self._fmt_read(ops[0])}, {self._fmt_read(ops[1])}, {ops[0].size*8}u)'
            return [self._fmt_write(ops[0], v)+';']
        if base in ('bt','bts','btr','btc'):
            if len(ops) != 2 or ops[0].size not in (2,4): raise ValueError('Unsupported bit test form')
            return self._bit_test(insn, base, ops)
        if base in ('bsf','bsr'):
            src = self._fmt_read(ops[1]); bits = ops[0].size*8
            return [f'{{ uint32_t bsv = {src}; if (bsv == 0) {{ eflags |= 0x40u; }} else {{ eflags &= ~0x40u; {self._fmt_write(ops[0], "engine_"+base+"(bsv)")}; }} }}']
        if base == 'xchg' and len(ops) == 2:
            return [f'{{ uint32_t first={self._fmt_read(ops[0])},second={self._fmt_read(ops[1])};',
                    self._fmt_write(ops[0],'second')+';', self._fmt_write(ops[1],'first')+'; }']
        if base == 'xadd' and len(ops) == 2:
            bits = ops[0].size*8
            return [f'{{ uint32_t dst={self._fmt_read(ops[0])}, src={self._fmt_read(ops[1])}; uint32_t sum=engine_flags_add(&eflags, dst, src, 0u, {bits}u);',
                    self._fmt_write(ops[1],'dst')+';', self._fmt_write(ops[0],'sum')+'; }']
        if base == 'cmpxchg' and len(ops) == 2:
            bits = ops[0].size*8; acc = {8:'LO8(eax)',16:'LO16(eax)',32:'eax'}[bits]
            setacc = {8:'SET_LO8(eax,{v})',16:'SET_LO16(eax,{v})',32:'eax=({v})'}[bits]
            return [f'{{ uint32_t dst={self._fmt_read(ops[0])}; (void)engine_flags_sub(&eflags, {acc}, dst, 0u, {bits}u);',
                    f'if (eflags & 0x40u) {{ {self._fmt_write(ops[0], self._fmt_read(ops[1]))}; }} else {{ {setacc.format(v="dst")}; }} }}']
        if base == 'xlatb':
            return ['SET_LO8(eax, engine_read_u8(cpu, ebx + LO8(eax)));']
        if base == 'pushal':
            return ['{ uint32_t saved_esp = esp; engine_push(cpu, eax, 4); engine_push(cpu, ecx, 4); engine_push(cpu, edx, 4); engine_push(cpu, ebx, 4); engine_push(cpu, saved_esp, 4); engine_push(cpu, ebp, 4); engine_push(cpu, esi, 4); engine_push(cpu, edi, 4); }']
        if base == 'popal':
            return ['edi = engine_pop(cpu, 4); esi = engine_pop(cpu, 4); ebp = engine_pop(cpu, 4); (void)engine_pop(cpu, 4); ebx = engine_pop(cpu, 4); edx = engine_pop(cpu, 4); ecx = engine_pop(cpu, 4); eax = engine_pop(cpu, 4);']
        if base in ('pushfd','popfd','pushf','popf'):
            if base.startswith('pushf'): return ['engine_push(cpu, eflags | 0x202u, 4);']
            return ['eflags = engine_pop(cpu, 4);']
        if base == 'cpuid':
            return ['engine_cpuid(cpu);']
        if base == 'rdtsc':
            return ['engine_rdtsc(cpu);']
        if base in ('emms','femms'):
            return ['/* MMX/3DNow! state release: no MMX state is modeled. */']
        if base == 'int3':
            return ['engine_fail(cpu, "int3 breakpoint reached");']
        if base == 'hlt' or base == 'ud2':
            return [f'engine_fail(cpu, "{base} executed");']
        if base == 'cwd':
            return ['SET_LO16(edx, (LO16(eax) & 0x8000u) ? 0xffffu : 0u);']
        if base == 'lock':
            return []
        return None
