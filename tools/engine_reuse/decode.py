"""Bounded recursive decoding, without repeatedly decoding past block terminators."""
from .upstream import Disassembler, Instruction, Function, BasicBlock


class DecodeError(ValueError):
    pass


class EngineDisassembler(Disassembler):
    def __init__(self, *args, instruction_budget=50000, **kwargs):
        super().__init__(*args, **kwargs)
        self.instruction_budget = instruction_budget
        self.instructions = {}

    def instruction_at(self, address):
        if address not in self.instructions:
            raw = self.read_bytes(address, 15)
            decoded = next(self.md.disasm(raw or b'', address, count=1), None)
            if decoded is None:
                raise DecodeError(f'Cannot decode instruction at {address:08X}')
            self.instructions[address] = Instruction(
                address=decoded.address, size=decoded.size,
                mnemonic=decoded.mnemonic, op_str=decoded.op_str,
                bytes=bytes(decoded.bytes), operands=list(decoded.operands))
        return self.instructions[address]

    def disassemble_function(self, start_va, iat_map=None, *, end_va=None,
                             extra_entries=(), known_functions=()):
        """Decode explicit edges; report indirect edges instead of guessing a switch.

        Known separate function entries and explicit end bounds delimit tail calls.
        The budget fails closed. No indirect branch target is silently invented.
        """
        if not self.is_code_address(start_va):
            raise DecodeError(f'Entry outside code: {start_va:08X}')
        func = Function(address=start_va, name=f'sub_{start_va:08X}')
        func.indirect_edges = []
        func.tail_calls = set()
        decoded = {}
        leaders = {start_va, *extra_entries}
        pending = list(leaders)
        foreign = set(known_functions) - {start_va}

        def internal(target):
            return (self.is_code_address(target) and target not in foreign and
                    (end_va is None or start_va <= target < end_va))

        while pending:
            address = pending.pop()
            while address not in decoded:
                if len(decoded) >= self.instruction_budget:
                    raise DecodeError(f'{start_va:08X} exceeds {self.instruction_budget} instructions')
                if not internal(address):
                    if address in foreign and address not in leaders:
                        # Adjacent CRT entries can share an epilogue by falling
                        # directly into the next function instead of using JMP.
                        func.tail_calls.add(address)
                        break
                    raise DecodeError(f'Fallthrough outside function at {address:08X}')
                insn = self.instruction_at(address)
                decoded[address] = insn
                target = insn.get_branch_target()
                if insn.is_call:
                    if target is not None:
                        func.calls_to.add(target)
                    else:
                        func.indirect_edges.append((address, 'call', insn.op_str))
                    # The return site is a resume point for setjmp/longjmp and SEH unwinding,
                    # so make it a basic-block leader (dispatchable), if it stays inside the function.
                    if internal(insn.end_address):
                        leaders.add(insn.end_address)
                        pending.append(insn.end_address)
                if insn.is_jump:
                    if target is None:
                        func.indirect_edges.append((address, 'jump', insn.op_str))
                    elif internal(target):
                        leaders.add(target)
                        pending.append(target)
                    else:
                        func.tail_calls.add(target)
                    if insn.is_cond_jump:
                        if internal(insn.end_address):
                            leaders.add(insn.end_address)
                            pending.append(insn.end_address)
                        else:
                            # Conditional fallthrough can enter a separately
                            # indexed epilogue just like an explicit JMP can.
                            func.tail_calls.add(insn.end_address)
                    break
                if insn.is_ret or insn.mnemonic == 'int3':
                    break
                address = insn.end_address

        # Every byte must belong to exactly one decoded instruction. Reject
        # accidental entry into the middle of another instruction.
        previous_end = None
        for address in sorted(decoded):
            if previous_end is not None and address < previous_end:
                raise DecodeError(f'Overlapping decode at {address:08X}')
            previous_end = decoded[address].end_address
        for leader in sorted(leaders):
            block = BasicBlock(start=leader, end=leader)
            address = leader
            while address in decoded:
                if address != leader and address in leaders:
                    block.successors.append(address)
                    break
                insn = decoded[address]
                block.instructions.append(insn)
                block.end = insn.end_address
                if insn.is_call and insn.end_address in leaders:
                    block.successors.append(insn.end_address)
                    break
                if insn.is_jump:
                    target = insn.get_branch_target()
                    if target is not None:
                        block.successors.append(target)
                    if insn.is_cond_jump:
                        block.successors.append(insn.end_address)
                    break
                if insn.is_ret or insn.mnemonic == 'int3':
                    block.is_exit = True
                    break
                address = insn.end_address
            if not block.instructions:
                raise DecodeError(f'Empty block at {leader:08X}')
            func.blocks[leader] = block
        func.end = max(block.end for block in func.blocks.values())
        func.size = func.end - start_va
        return func
