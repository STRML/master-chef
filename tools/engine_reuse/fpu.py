"""x87 semantics for the engine, computed in binary64.

Arithmetic runs in double regardless of the guest precision-control field.
Masked exceptions produce IEEE results (inf/NaN) as hardware would; only an
instruction the lifter cannot express fails translation.
"""
from capstone.x86 import X86_OP_REG, X86_OP_MEM, X86_REG_ST0, X86_REG_ST7

_CONSTANTS = {'fldz':'0.0','fld1':'1.0','fldpi':'3.14159265358979323846',
              'fldl2e':'1.44269504088896340736','fldl2t':'3.32192809488736234787',
              'fldlg2':'0.30102999566398119521','fldln2':'0.69314718055994530942'}
_TRANS = {'fsin':'ENGINE_FP_FSIN','fcos':'ENGINE_FP_FCOS','fsincos':'ENGINE_FP_FSINCOS',
          'fptan':'ENGINE_FP_FPTAN','fpatan':'ENGINE_FP_FPATAN','fyl2x':'ENGINE_FP_FYL2X',
          'fyl2xp1':'ENGINE_FP_FYL2XP1','f2xm1':'ENGINE_FP_F2XM1','fscale':'ENGINE_FP_FSCALE',
          'fprem':'ENGINE_FP_FPREM','fprem1':'ENGINE_FP_FPREM1','fxam':'ENGINE_FP_FXAM',
          'fxtract':'ENGINE_FP_FXTRACT'}


class FPUMixin:
    def _fp_index(self, operand):
        if operand.type != X86_OP_REG or not X86_REG_ST0 <= operand.reg <= X86_REG_ST7:
            raise ValueError('Expected x87 stack register')
        return operand.reg - X86_REG_ST0

    def _fp_read(self, operand):
        if operand.type == X86_OP_REG:
            return f'engine_fp_read(cpu, {self._fp_index(operand)})'
        if operand.type == X86_OP_MEM and operand.size in (4, 8, 10):
            addr = self._fmt_mem_addr(operand.mem)
            return f'engine_read_f{operand.size*8}(cpu, {addr})'
        raise ValueError('Unsupported floating operand')

    def lift_fpu_instruction(self, insn):
        m, ops = insn.mnemonic, insn.operands or []
        if m in ('wait', 'fwait'):
            return ['/* Masked x87 exceptions are checked by each operation. */']
        if not m.startswith('f'):
            return None
        if m in _CONSTANTS:
            return [f'engine_fp_push(cpu, {_CONSTANTS[m]});']
        if m == 'fld':
            return [f'engine_fp_push(cpu, {self._fp_read(ops[0])});']
        if m == 'fild':
            if len(ops) != 1 or ops[0].type != X86_OP_MEM or ops[0].size not in (2,4,8):
                raise ValueError('Unsupported FILD encoding')
            return [f'engine_fp_load_integer(cpu, {self._fmt_mem_addr(ops[0].mem)}, {ops[0].size});']
        if m in ('fst', 'fstp'):
            op = ops[0]
            if op.type == X86_OP_REG:
                lines = [f'engine_fp_write(cpu, {self._fp_index(op)}, engine_fp_read(cpu, 0));']
            elif op.type == X86_OP_MEM and op.size in (4,8,10):
                lines = [f'engine_write_f{op.size*8}(cpu, {self._fmt_mem_addr(op.mem)}, engine_fp_read(cpu, 0));']
            else:
                raise ValueError('Unsupported FST encoding')
            return lines + (['engine_fp_pop(cpu);'] if m == 'fstp' else [])
        if m in ('fist', 'fistp', 'fisttp'):
            op = ops[0]
            if op.type != X86_OP_MEM or op.size not in (2,4,8):
                raise ValueError('Unsupported FIST encoding')
            return [f'engine_fp_store_integer(cpu, {self._fmt_mem_addr(op.mem)}, {op.size}, {int(m == "fisttp")});'] + (['engine_fp_pop(cpu);'] if m != 'fist' else [])
        arithmetic = {'fadd':'+','fmul':'*','fsub':'-','fsubr':'-', 'fdiv':'/','fdivr':'/'}
        integer = {'fiadd':'fadd','fimul':'fmul','fisub':'fsub','fisubr':'fsubr','fidiv':'fdiv','fidivr':'fdivr'}
        if m in integer:
            op = ops[0]
            if op.type != X86_OP_MEM or op.size not in (2,4):
                raise ValueError('Unsupported integer x87 operand')
            base = integer[m]
            left, right = 'engine_fp_read(cpu, 0)', f'engine_fp_integer_operand(cpu, {self._fmt_mem_addr(op.mem)}, {op.size})'
            if base.endswith('r'):
                left, right = right, left
            opname = {'+':'ADD','-':'SUB','*':'MUL','/':'DIV'}[arithmetic[base]]
            return [f'engine_fp_write(cpu, 0, engine_fp_arithmetic(cpu, ENGINE_FP_{opname}, {left}, {right}));']
        base = m[:-1] if m.endswith('p') else m
        if base in arithmetic:
            pop = m.endswith('p')
            if pop:
                destination = self._fp_index(ops[0]) if ops else 1
                left, right = f'engine_fp_read(cpu, {destination})', 'engine_fp_read(cpu, 0)'
            elif len(ops) == 2:
                destination = self._fp_index(ops[0])
                left, right = self._fp_read(ops[0]), self._fp_read(ops[1])
            else:
                destination = 0
                left = 'engine_fp_read(cpu, 0)'
                right = self._fp_read(ops[0]) if ops else 'engine_fp_read(cpu, 1)'
            if base.endswith('r'):
                left, right = right, left
            op = {'+':'ADD','-':'SUB','*':'MUL','/':'DIV'}[arithmetic[base]]
            return [f'engine_fp_write(cpu, {destination}, engine_fp_arithmetic(cpu, ENGINE_FP_{op}, {left}, {right}));'] + (['engine_fp_pop(cpu);'] if pop else [])
        if m in ('fcom','fcomp','fcompp','fucom','fucomp','fucompp','fcomi','fcomip','fcompi','fucomi','fucomip','fucompi'):
            src = self._fp_read(ops[-1]) if ops else 'engine_fp_read(cpu, 1)'
            integer_flags = 'i' in m
            count = 2 if m.endswith('pp') else 1 if m.endswith(('p','pi')) else 0
            return [f'engine_fp_compare(cpu, engine_fp_read(cpu, 0), {src}, {int(integer_flags)});'] + ['engine_fp_pop(cpu);']*count
        if m in ('ficom','ficomp'):
            op = ops[0]
            if op.type != X86_OP_MEM or op.size not in (2,4):
                raise ValueError('Unsupported FICOM operand')
            return [f'engine_fp_compare(cpu, engine_fp_read(cpu, 0), engine_fp_integer_operand(cpu, {self._fmt_mem_addr(op.mem)}, {op.size}), 0);'] + (['engine_fp_pop(cpu);'] if m.endswith('p') else [])
        if m in ('ftst',):
            return ['engine_fp_compare(cpu, engine_fp_read(cpu, 0), 0.0, 0);']
        if m in ('fnstsw','fstsw'):
            return [self._fmt_write(ops[0], 'engine_fp_status(cpu)')+';']
        if m in ('fnstcw','fstcw'):
            return [self._fmt_write(ops[0], 'cpu->fp_control')+';']
        if m == 'fldcw':
            return [f'engine_fp_control(cpu, {self._fmt_read(ops[0])});']
        if m in ('fninit','finit'):
            return ['engine_fp_init(cpu);']
        if m in ('fnclex','fclex'):
            return ['cpu->fp_status &= (uint16_t)~0x80ffu;']
        if m == 'fxch':
            index = self._fp_index(ops[-1]) if ops else 1
            return [f'engine_fp_exchange(cpu, {index});']
        if m == 'ffree':
            return [f'engine_fp_free(cpu, {self._fp_index(ops[0])});']
        if m in ('fchs','fabs','fsqrt','frndint'):
            return [f'engine_fp_unary(cpu, ENGINE_FP_{m.upper()});']
        if m in _TRANS:
            return [f'engine_fp_transcendental(cpu, {_TRANS[m]});']
        if m.startswith('fcmov'):
            index = self._fp_index(ops[-1])
            cond = {'fcmovb':'b','fcmove':'e','fcmovbe':'be','fcmovu':'p','fcmovnb':'ae','fcmovne':'ne','fcmovnbe':'a','fcmovnu':'np'}[m]
            return [f'if ({self.flags_condition(cond)}) {{ engine_fp_write(cpu, 0, engine_fp_read(cpu, {index})); }}']
        if m in ('fnstenv','fstenv','fldenv','fnsave','fsave','frstor'):
            op = ops[0]
            if op.type != X86_OP_MEM:
                raise ValueError('x87 environment operand must be memory')
            fn = {'fnstenv':'engine_fp_store_environment','fstenv':'engine_fp_store_environment',
                  'fldenv':'engine_fp_load_environment','fnsave':'engine_fp_save','fsave':'engine_fp_save',
                  'frstor':'engine_fp_restore'}[m]
            return [f'{fn}(cpu, {self._fmt_mem_addr(op.mem)});']
        raise ValueError(f'Unsupported x87 operation: {m} {insn.op_str}')
