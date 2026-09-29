"""Reusable integer-EFLAGS lowering for the Halo static recompiler.

The mixin records architectural flags in the shared C ``eflags`` word at the
instruction that produces them.  It deliberately does not infer a later branch
condition from nearby operands, so calls, basic-block boundaries, and unrelated
register writes do not destroy the condition state.
"""

from __future__ import annotations

from typing import Any


_BINARY = {"add", "sub", "adc", "sbb", "and", "or", "xor", "cmp", "test"}
_SHIFT = {"sal", "sar", "shl", "shr"}
_UNARY = {"inc", "dec", "neg"}
_OWNED = _BINARY | _SHIFT | _UNARY | {"imul"}

# These instructions write or undefine some integer flags.  Returning None for
# them would allow the prototype's operand-history heuristic to run, which is a
# silent miscompile.  Dedicated lowering can remove names from this set later.
_KNOWN_UNSUPPORTED_FLAG_WRITERS = {
    "aaa", "aad", "aam", "aas", "bt", "btc", "btr", "bts", "clc", "cmc",
    "cmpsb", "cmpsd", "cmpsw", "cmpxchg", "cmpxchg8b", "daa", "das",
    "div", "idiv", "mul", "popf", "popfd", "rcl", "rcr", "rol", "ror",
    "sahf", "scasb", "scasd", "scasw", "shld", "shrd", "stc", "xadd",
}

_FLAG_BITS = {
    "cf": "ENGINE_EFLAGS_CF",
    "pf": "ENGINE_EFLAGS_PF",
    "af": "ENGINE_EFLAGS_AF",
    "zf": "ENGINE_EFLAGS_ZF",
    "sf": "ENGINE_EFLAGS_SF",
    "of": "ENGINE_EFLAGS_OF",
}

UNDEFINED_FLAG_NOTES = {
    "and/or/xor/test": "AF is undefined; the helper clears it",
    "shl/shr/sar nonzero": "AF is undefined; the helpers clear it",
    "shl/shr/sar count >= width": "CF is undefined; the helpers preserve it",
    "shl/shr/sar count > 1": "OF is undefined; the helpers preserve it",
    "imul two/three operand": "PF/AF/ZF/SF are undefined; the helper preserves them",
    "div/idiv": "all arithmetic status flags are undefined; lowering is rejected",
}

_ALIASES = {
    "o": "o", "no": "no",
    "b": "b", "c": "b", "nae": "b",
    "ae": "ae", "nb": "ae", "nc": "ae",
    "e": "e", "z": "e", "ne": "ne", "nz": "ne",
    "be": "be", "na": "be", "a": "a", "nbe": "a",
    "s": "s", "ns": "ns",
    "p": "p", "pe": "p", "np": "np", "po": "np",
    "l": "l", "nge": "l", "ge": "ge", "nl": "ge",
    "le": "le", "ng": "le", "g": "g", "nle": "g",
}


class FlagsMixin:
    """Lower width-correct IA-32 integer flag producers and consumers.

    Preferred host hooks are ``_read_op(op, insn)`` and
    ``_write_op(op, value, insn)``.  The pinned prototype currently calls the
    same operations ``_fmt_read(op)`` and ``_fmt_write(op, value)``; the small
    adapters below retain compatibility without coupling flag semantics to it.
    """

    @staticmethod
    def _flags_mnemonic(insn: Any) -> str:
        # Capstone includes textual prefixes ("lock add", "repe cmpsb") in
        # mnemonic.  The pinned Instruction wrapper retains that string but not
        # its separate prefix byte array.
        tokens = str(getattr(insn, "mnemonic", "")).lower().split()
        return tokens[-1] if tokens else ""

    def _flags_read(self, op: Any, insn: Any) -> str:
        reader = getattr(self, "_read_op", None)
        if reader is not None:
            return reader(op, insn)
        reader = getattr(self, "_fmt_read", None)
        if reader is not None:
            return reader(op)
        raise TypeError("FlagsMixin host must provide _read_op(op, insn)")

    def _flags_write(self, op: Any, value: str, insn: Any) -> str:
        writer = getattr(self, "_write_op", None)
        if writer is not None:
            return writer(op, value, insn)
        writer = getattr(self, "_fmt_write", None)
        if writer is not None:
            return writer(op, value)
        raise TypeError("FlagsMixin host must provide _write_op(op, value, insn)")

    @staticmethod
    def _flags_width(op: Any, insn: Any) -> int:
        size = int(getattr(op, "size", 0) or 0)
        if size not in (1, 2, 4):
            address = int(getattr(insn, "address", 0))
            raise ValueError(
                f"unsupported flag operand width {size * 8} at 0x{address:08X}"
            )
        return size * 8

    @staticmethod
    def _flags_check_prefix(insn: Any) -> None:
        prefixes = tuple(int(value) for value in (getattr(insn, "prefix", ()) or ()))
        mnemonic = str(getattr(insn, "mnemonic", "")).lower()
        if 0xF0 in prefixes or mnemonic.startswith("lock "):
            address = int(getattr(insn, "address", 0))
            raise ValueError(
                f"LOCK flag instruction requires atomic lowering at 0x{address:08X}"
            )

    def lift_flag_instruction(self, insn: Any) -> list[str] | None:
        """Return C statements for a supported flag-writing instruction.

        ``None`` means the instruction is flag-neutral or belongs to another
        mixin.  Recognized flag-writing instructions outside this module raise
        ``NotImplementedError`` so the heuristic upstream implementation cannot
        silently handle them.
        """
        mnemonic = self._flags_mnemonic(insn)
        if mnemonic not in _OWNED:
            if mnemonic in _KNOWN_UNSUPPORTED_FLAG_WRITERS:
                address = int(getattr(insn, "address", 0))
                raise NotImplementedError(
                    f"{mnemonic} changes EFLAGS but has no exact lowering "
                    f"at 0x{address:08X}"
                )
            return None

        self._flags_check_prefix(insn)
        ops = list(getattr(insn, "operands", ()) or ())
        if mnemonic == "imul":
            if len(ops) not in (2, 3):
                address = int(getattr(insn, "address", 0))
                raise NotImplementedError(
                    f"only two/three-operand IMUL is supported at 0x{address:08X}"
                )
            width = self._flags_width(ops[0], insn)
            if width not in (16, 32):
                address = int(getattr(insn, "address", 0))
                raise ValueError(
                    f"unsupported {width}-bit IMUL at 0x{address:08X}"
                )
            sources = ops if len(ops) == 2 else ops[1:]
            for source in sources:
                source_size = int(getattr(source, "size", 0) or 0)
                if source_size not in (0, width // 8):
                    address = int(getattr(insn, "address", 0))
                    raise ValueError(
                        f"mismatched IMUL operand widths at 0x{address:08X}"
                    )
            lhs = self._flags_read(sources[0], insn)
            rhs = self._flags_read(sources[1], insn)
            value = f"engine_flags_imul(&eflags, {lhs}, {rhs}, {width}u)"
            statement = self._flags_write(ops[0], value, insn).rstrip()
            return [statement if statement.endswith(";") else statement + ";"]

        expected = 2 if mnemonic in (_BINARY | _SHIFT) else 1
        if len(ops) != expected:
            address = int(getattr(insn, "address", 0))
            raise ValueError(
                f"invalid {mnemonic} operand count {len(ops)} at 0x{address:08X}"
            )

        width = self._flags_width(ops[0], insn)
        if len(ops) == 2 and mnemonic not in _SHIFT:
            rhs_size = int(getattr(ops[1], "size", 0) or 0)
            if rhs_size not in (0, width // 8):
                address = int(getattr(insn, "address", 0))
                raise ValueError(
                    f"mismatched {mnemonic} operand widths at 0x{address:08X}"
                )

        lhs = self._flags_read(ops[0], insn)
        rhs = self._flags_read(ops[1], insn) if len(ops) == 2 else None
        if mnemonic == "cmp":
            return [f"(void)engine_flags_sub(&eflags, {lhs}, {rhs}, 0u, {width}u);"]
        if mnemonic == "test":
            return [f"(void)engine_flags_logic(&eflags, ({lhs}) & ({rhs}), {width}u);"]
        if mnemonic in _SHIFT:
            helper = "shl" if mnemonic == "sal" else mnemonic
            value = f"engine_flags_{helper}(&eflags, {lhs}, {rhs}, {width}u)"
            statement = self._flags_write(ops[0], value, insn).rstrip()
            return [statement if statement.endswith(";") else statement + ";"]

        if mnemonic in ("add", "sub"):
            helper = f"engine_flags_{mnemonic}"
            value = f"{helper}(&eflags, {lhs}, {rhs}, 0u, {width}u)"
        elif mnemonic in ("adc", "sbb"):
            helper = "engine_flags_add" if mnemonic == "adc" else "engine_flags_sub"
            value = (
                f"{helper}(&eflags, {lhs}, {rhs}, "
                f"(eflags & ENGINE_EFLAGS_CF), {width}u)"
            )
        elif mnemonic in ("and", "or", "xor"):
            operator = {"and": "&", "or": "|", "xor": "^"}[mnemonic]
            value = f"engine_flags_logic(&eflags, ({lhs}) {operator} ({rhs}), {width}u)"
        else:
            value = f"engine_flags_{mnemonic}(&eflags, {lhs}, {width}u)"

        statement = self._flags_write(ops[0], value, insn).rstrip()
        if not statement.endswith(";"):
            statement += ";"
        return [statement]

    def flags_condition(self, mnemonic: str) -> str:
        """Return a C expression for Jcc, SETcc, or CMOVcc ``mnemonic``."""
        name = mnemonic.lower()
        if name.startswith("cmov"):
            suffix = name[4:]
        elif name.startswith("set"):
            suffix = name[3:]
        elif name.startswith("j"):
            suffix = name[1:]
        else:
            suffix = name
        condition = _ALIASES.get(suffix)
        if condition is None:
            raise ValueError(f"unsupported EFLAGS condition mnemonic: {mnemonic}")

        bit = lambda flag: f"((eflags & {_FLAG_BITS[flag]}) != 0u)"
        same_sign = f"({bit('sf')} == {bit('of')})"
        different_sign = f"({bit('sf')} != {bit('of')})"
        expressions = {
            "o": bit("of"), "no": f"!{bit('of')}",
            "b": bit("cf"), "ae": f"!{bit('cf')}",
            "e": bit("zf"), "ne": f"!{bit('zf')}",
            "be": f"({bit('cf')} || {bit('zf')})",
            "a": f"(!{bit('cf')} && !{bit('zf')})",
            "s": bit("sf"), "ns": f"!{bit('sf')}",
            "p": bit("pf"), "np": f"!{bit('pf')}",
            "l": different_sign, "ge": same_sign,
            "le": f"({bit('zf')} || {different_sign})",
            "g": f"(!{bit('zf')} && {same_sign})",
        }
        return expressions[condition]


__all__ = ["FlagsMixin", "UNDEFINED_FLAG_NOTES"]
