#!/usr/bin/env python3
"""Differential tests for the reusable IA-32 integer flags layer."""

from __future__ import annotations

import ctypes
import random
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EFLAGS

try:
    from .flags import FlagsMixin, UNDEFINED_FLAG_NOTES
except ImportError:  # Direct execution: python3 tools/engine_reuse/test_flags.py
    from flags import FlagsMixin, UNDEFINED_FLAG_NOTES


ROOT = Path(__file__).resolve().parents[2]
HEADER_DIR = ROOT / "native" / "EngineReuse"

CF = 0x0001
PF = 0x0004
AF = 0x0010
ZF = 0x0040
SF = 0x0080
DF = 0x0400
OF = 0x0800
STATUS = CF | PF | AF | ZF | SF | OF
LOGIC_STATUS = CF | PF | ZF | SF | OF

OPS = (
    "add", "sub", "adc", "sbb", "and", "or", "xor", "cmp", "test",
    "inc", "dec", "neg",
)

ENCODINGS = {
    ("add", 8): b"\x00\xc8", ("add", 16): b"\x66\x01\xc8", ("add", 32): b"\x01\xc8",
    ("sub", 8): b"\x28\xc8", ("sub", 16): b"\x66\x29\xc8", ("sub", 32): b"\x29\xc8",
    ("adc", 8): b"\x10\xc8", ("adc", 16): b"\x66\x11\xc8", ("adc", 32): b"\x11\xc8",
    ("sbb", 8): b"\x18\xc8", ("sbb", 16): b"\x66\x19\xc8", ("sbb", 32): b"\x19\xc8",
    ("and", 8): b"\x20\xc8", ("and", 16): b"\x66\x21\xc8", ("and", 32): b"\x21\xc8",
    ("or", 8): b"\x08\xc8", ("or", 16): b"\x66\x09\xc8", ("or", 32): b"\x09\xc8",
    ("xor", 8): b"\x30\xc8", ("xor", 16): b"\x66\x31\xc8", ("xor", 32): b"\x31\xc8",
    ("cmp", 8): b"\x38\xc8", ("cmp", 16): b"\x66\x39\xc8", ("cmp", 32): b"\x39\xc8",
    ("test", 8): b"\x84\xc8", ("test", 16): b"\x66\x85\xc8", ("test", 32): b"\x85\xc8",
    ("inc", 8): b"\xfe\xc0", ("inc", 16): b"\x66\x40", ("inc", 32): b"\x40",
    ("dec", 8): b"\xfe\xc8", ("dec", 16): b"\x66\x48", ("dec", 32): b"\x48",
    ("neg", 8): b"\xf6\xd8", ("neg", 16): b"\x66\xf7\xd8", ("neg", 32): b"\xf7\xd8",
}

SHIFT_ENCODINGS = {
    ("shl", 8): b"\xd2\xe0", ("shl", 16): b"\x66\xd3\xe0", ("shl", 32): b"\xd3\xe0",
    ("shr", 8): b"\xd2\xe8", ("shr", 16): b"\x66\xd3\xe8", ("shr", 32): b"\xd3\xe8",
    ("sar", 8): b"\xd2\xf8", ("sar", 16): b"\x66\xd3\xf8", ("sar", 32): b"\xd3\xf8",
}

IMUL2_ENCODINGS = {
    16: b"\x66\x0f\xaf\xc1",
    32: b"\x0f\xaf\xc1",
}

OP_INDEX = {name: index for index, name in enumerate(OPS)}
CONDITIONS = (
    "jo", "jno", "jb", "jae", "je", "jne", "jbe", "ja", "js", "jns",
    "jp", "jnp", "jl", "jge", "jle", "jg",
)


class _ConditionHost(FlagsMixin):
    pass


def c_source() -> str:
    host = _ConditionHost()
    condition_cases = "\n".join(
        f"        case {index}u: return ({host.flags_condition(name)}) ? 1u : 0u;"
        for index, name in enumerate(CONDITIONS)
    )
    return f'''#include <stdint.h>
#include "engine_flags.h"

uint32_t eflags = ENGINE_EFLAGS_RESERVED1;

uint64_t flags_run(uint32_t op, uint32_t width, uint32_t lhs,
                   uint32_t rhs, uint32_t initial_flags) {{
    uint32_t result = lhs & engine_flags_width_mask(width);
    eflags = initial_flags;
    switch (op) {{
        case 0u: result = engine_flags_add(&eflags, lhs, rhs, 0u, width); break;
        case 1u: result = engine_flags_sub(&eflags, lhs, rhs, 0u, width); break;
        case 2u: result = engine_flags_add(&eflags, lhs, rhs, eflags & ENGINE_EFLAGS_CF, width); break;
        case 3u: result = engine_flags_sub(&eflags, lhs, rhs, eflags & ENGINE_EFLAGS_CF, width); break;
        case 4u: result = engine_flags_logic(&eflags, lhs & rhs, width); break;
        case 5u: result = engine_flags_logic(&eflags, lhs | rhs, width); break;
        case 6u: result = engine_flags_logic(&eflags, lhs ^ rhs, width); break;
        case 7u: (void)engine_flags_sub(&eflags, lhs, rhs, 0u, width); break;
        case 8u: (void)engine_flags_logic(&eflags, lhs & rhs, width); break;
        case 9u: result = engine_flags_inc(&eflags, lhs, width); break;
        case 10u: result = engine_flags_dec(&eflags, lhs, width); break;
        case 11u: result = engine_flags_neg(&eflags, lhs, width); break;
        default: return UINT64_MAX;
    }}
    return ((uint64_t)eflags << 32u) | result;
}}

uint64_t flags_shift_run(uint32_t op, uint32_t width, uint32_t value,
                         uint32_t count, uint32_t initial_flags) {{
    uint32_t result;
    eflags = initial_flags;
    switch (op) {{
        case 0u: result = engine_flags_shl(&eflags, value, count, width); break;
        case 1u: result = engine_flags_shr(&eflags, value, count, width); break;
        case 2u: result = engine_flags_sar(&eflags, value, count, width); break;
        default: return UINT64_MAX;
    }}
    return ((uint64_t)eflags << 32u) | result;
}}

uint64_t flags_imul_run(uint32_t width, uint32_t lhs, uint32_t rhs,
                        uint32_t initial_flags) {{
    uint32_t result;
    eflags = initial_flags;
    result = engine_flags_imul(&eflags, lhs, rhs, width);
    return ((uint64_t)eflags << 32u) | result;
}}

uint32_t flags_condition_peek(uint32_t condition) {{
    switch (condition) {{
{condition_cases}
        default: return UINT32_MAX;
    }}
}}

uint32_t flags_condition_run(uint32_t condition, uint32_t input_flags) {{
    eflags = input_flags;
    return flags_condition_peek(condition);
}}
'''


class NativeLibrary:
    def __init__(self, optimization: str):
        self.temp = tempfile.TemporaryDirectory(prefix="halo-flags-")
        directory = Path(self.temp.name)
        source = directory / "flags_test.c"
        suffix = ".dylib" if sys.platform == "darwin" else ".so"
        library = directory / f"flags_test{suffix}"
        source.write_text(c_source(), encoding="utf-8")
        link_flag = "-dynamiclib" if sys.platform == "darwin" else "-shared"
        compiler = shutil.which("clang") or shutil.which("cc")
        if compiler is None:
            raise unittest.SkipTest("no C compiler available")
        subprocess.run(
            [compiler, "-std=c11", optimization, "-fPIC", link_flag,
             "-Wall", "-Wextra", "-Werror", "-I", str(HEADER_DIR),
             str(source), "-o", str(library)],
            check=True, capture_output=True, text=True,
        )
        self.lib = ctypes.CDLL(str(library))
        self.lib.flags_run.argtypes = [ctypes.c_uint32] * 5
        self.lib.flags_run.restype = ctypes.c_uint64
        self.lib.flags_condition_run.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
        self.lib.flags_condition_run.restype = ctypes.c_uint32
        self.lib.flags_condition_peek.argtypes = [ctypes.c_uint32]
        self.lib.flags_condition_peek.restype = ctypes.c_uint32
        self.lib.flags_shift_run.argtypes = [ctypes.c_uint32] * 5
        self.lib.flags_shift_run.restype = ctypes.c_uint64
        self.lib.flags_imul_run.argtypes = [ctypes.c_uint32] * 4
        self.lib.flags_imul_run.restype = ctypes.c_uint64

    def close(self) -> None:
        self.temp.cleanup()


class UnicornInstruction:
    BASE = 0x100000

    def __init__(self, code: bytes):
        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        self.uc.mem_map(self.BASE, 0x1000)
        self.uc.mem_write(self.BASE, code)
        self.end = self.BASE + len(code)

    def run(self, lhs: int, rhs: int, initial_flags: int) -> tuple[int, int]:
        self.uc.reg_write(UC_X86_REG_EAX, lhs)
        self.uc.reg_write(UC_X86_REG_ECX, rhs)
        self.uc.reg_write(UC_X86_REG_EFLAGS, initial_flags)
        self.uc.emu_start(self.BASE, self.end)
        return (
            self.uc.reg_read(UC_X86_REG_EAX),
            self.uc.reg_read(UC_X86_REG_EFLAGS),
        )


class FlagsDifferentialTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.native = {
            "debug": NativeLibrary("-O0"),
            "optimized": NativeLibrary("-O2"),
        }
        cls.unicorn = {key: UnicornInstruction(code) for key, code in ENCODINGS.items()}
        cls.shift_unicorn = {
            key: UnicornInstruction(code) for key, code in SHIFT_ENCODINGS.items()
        }
        cls.imul2_unicorn = {
            width: UnicornInstruction(code) for width, code in IMUL2_ENCODINGS.items()
        }
        cls.imul3_unicorn = {}

    @classmethod
    def tearDownClass(cls) -> None:
        for native in cls.native.values():
            native.close()

    @staticmethod
    def cases(width: int, mnemonic: str):
        mask = (1 << width) - 1
        sign = 1 << (width - 1)
        edges = (0, 1, 2, sign - 1, sign, sign + 1, mask - 1, mask)
        # Twenty-five directed edge pairs plus seventy-five deterministic random pairs.
        for index in range(25):
            yield edges[index % len(edges)], edges[(index * 3 + 1) % len(edges)], index & 1
        rng = random.Random(0x48414C4F ^ width ^ OP_INDEX[mnemonic])
        for _ in range(75):
            yield rng.getrandbits(width), rng.getrandbits(width), rng.getrandbits(1)

    def test_helpers_match_unchanged_x86_bytes(self) -> None:
        receipt = 0
        for build, native in self.native.items():
            for mnemonic in OPS:
                for width in (8, 16, 32):
                    mask = (1 << width) - 1
                    emulator = self.unicorn[(mnemonic, width)]
                    for lhs, rhs, carry in self.cases(width, mnemonic):
                        # Preserve DF and seed every status bit, while bit 1 remains fixed.
                        initial = 0x0002 | DF | (STATUS if carry else 0)
                        x86_result, x86_flags = emulator.run(lhs, rhs, initial)
                        packed = native.lib.flags_run(
                            OP_INDEX[mnemonic], width, lhs, rhs, initial
                        )
                        c_result = packed & 0xFFFFFFFF
                        c_flags = packed >> 32
                        compared = LOGIC_STATUS if mnemonic in ("and", "or", "xor", "test") else STATUS
                        with self.subTest(build=build, op=mnemonic, width=width,
                                          lhs=lhs, rhs=rhs, carry=carry):
                            self.assertEqual(c_result & mask, x86_result & mask)
                            self.assertEqual(c_flags & (compared | DF | 0x0002),
                                             x86_flags & (compared | DF | 0x0002))
                        receipt += 1
        print(f"RECEIPT FLAGS-001: {receipt} C-vs-Unicorn instruction cases passed")

    def test_emitted_conditions_cover_all_status_combinations(self) -> None:
        def expected(name: str, flags: int) -> bool:
            cf, pf, zf = bool(flags & CF), bool(flags & PF), bool(flags & ZF)
            sf, of = bool(flags & SF), bool(flags & OF)
            return {
                "jo": of, "jno": not of, "jb": cf, "jae": not cf,
                "je": zf, "jne": not zf, "jbe": cf or zf,
                "ja": not cf and not zf, "js": sf, "jns": not sf,
                "jp": pf, "jnp": not pf, "jl": sf != of,
                "jge": sf == of, "jle": zf or sf != of,
                "jg": not zf and sf == of,
            }[name]

        receipt = 0
        native = self.native["optimized"]
        for bits in range(32):
            flags = ((bits & 1) * CF | ((bits >> 1) & 1) * PF |
                     ((bits >> 2) & 1) * ZF | ((bits >> 3) & 1) * SF |
                     ((bits >> 4) & 1) * OF)
            for index, name in enumerate(CONDITIONS):
                self.assertEqual(bool(native.lib.flags_condition_run(index, flags)),
                                 expected(name, flags), (name, flags))
                receipt += 1
        print(f"RECEIPT FLAGS-002: {receipt} emitted Jcc condition cases passed")

    def test_shared_flags_survive_a_call_boundary(self) -> None:
        jb = CONDITIONS.index("jb")
        je = CONDITIONS.index("je")
        for build, native in self.native.items():
            native.lib.flags_run(OP_INDEX["cmp"], 32, 1, 2, 0x0002)
            self.assertEqual(native.lib.flags_condition_peek(jb), 1, build)
            self.assertEqual(native.lib.flags_condition_peek(je), 0, build)
            native.lib.flags_run(OP_INDEX["cmp"], 32, 9, 9, 0x0002)
            self.assertEqual(native.lib.flags_condition_peek(jb), 0, build)
            self.assertEqual(native.lib.flags_condition_peek(je), 1, build)
        print("RECEIPT FLAGS-005: shared EFLAGS survived producer/consumer call boundaries")

    def test_shift_helpers_match_masked_count_x86_semantics(self) -> None:
        receipt = 0
        shift_index = {"shl": 0, "shr": 1, "sar": 2}
        directed_counts = (0, 1, 2, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 255)
        for build, native in self.native.items():
            for mnemonic in ("shl", "shr", "sar"):
                for width in (8, 16, 32):
                    mask = (1 << width) - 1
                    sign = 1 << (width - 1)
                    values = (0, 1, sign - 1, sign, sign + 1, mask - 1, mask)
                    cases = [(value, count) for value in values
                             for count in directed_counts]
                    rng = random.Random(0x53484946 ^ width ^ shift_index[mnemonic])
                    cases += [(rng.getrandbits(width), rng.getrandbits(8))
                              for _ in range(82)]
                    emulator = self.shift_unicorn[(mnemonic, width)]
                    for case_index, (value, count) in enumerate(cases):
                        initial = 0x0002 | DF | (STATUS if case_index & 1 else 0)
                        x86_result, x86_flags = emulator.run(value, count, initial)
                        packed = native.lib.flags_shift_run(
                            shift_index[mnemonic], width, value, count, initial
                        )
                        c_result, c_flags = packed & 0xFFFFFFFF, packed >> 32
                        masked_count = count & 31
                        if masked_count == 0:
                            compared = STATUS
                        else:
                            compared = PF | ZF | SF
                            if masked_count < width:
                                compared |= CF
                            if masked_count == 1:
                                compared |= OF
                        with self.subTest(build=build, op=mnemonic, width=width,
                                          value=value, count=count):
                            self.assertEqual(c_result & mask, x86_result & mask)
                            self.assertEqual(c_flags & (compared | DF | 0x0002),
                                             x86_flags & (compared | DF | 0x0002))
                        receipt += 1
        print(f"RECEIPT FLAGS-006: {receipt} masked-shift C-vs-Unicorn cases passed")

    def test_imul_two_and_three_operand_forms_match_x86(self) -> None:
        receipt = 0
        for width in (16, 32):
            mask = (1 << width) - 1
            sign = 1 << (width - 1)
            edges = (0, 1, 2, sign - 1, sign, sign + 1, mask - 1, mask)
            pairs = [(edges[index % len(edges)], edges[(index * 5 + 1) % len(edges)])
                     for index in range(32)]
            rng = random.Random(0x494D554C ^ width)
            pairs += [(rng.getrandbits(width), rng.getrandbits(width))
                      for _ in range(68)]
            for form in (2, 3):
                for lhs, rhs in pairs:
                    if form == 2:
                        emulator = self.imul2_unicorn[width]
                    else:
                        immediate = int(rhs & mask).to_bytes(width // 8, "little")
                        code = (b"\x66" if width == 16 else b"") + b"\x69\xc1" + immediate
                        emulator = self.imul3_unicorn.setdefault(
                            (width, rhs), UnicornInstruction(code)
                        )
                    for build, native in self.native.items():
                        initial = 0x0002 | DF | STATUS
                        if form == 2:
                            x86_result, x86_flags = emulator.run(lhs, rhs, initial)
                        else:
                            # Three-operand IMUL ignores the old destination EAX.
                            x86_result, x86_flags = emulator.run(mask ^ lhs, lhs, initial)
                        packed = native.lib.flags_imul_run(width, lhs, rhs, initial)
                        c_result, c_flags = packed & 0xFFFFFFFF, packed >> 32
                        with self.subTest(build=build, form=form, width=width,
                                          lhs=lhs, rhs=rhs):
                            self.assertEqual(c_result & mask, x86_result & mask)
                            # IMUL defines only CF and OF. PF/AF/ZF/SF are undefined.
                            self.assertEqual(c_flags & (CF | OF | DF | 0x0002),
                                             x86_flags & (CF | OF | DF | 0x0002))
                        receipt += 1
        print(f"RECEIPT FLAGS-007: {receipt} two/three-operand IMUL cases passed")


class FlagsMixinContractTests(unittest.TestCase):
    class Host(FlagsMixin):
        def __init__(self):
            self.calls = []

        def _read_op(self, op, insn):
            self.calls.append(("read", op, insn))
            return op.name

        def _write_op(self, op, value, insn):
            self.calls.append(("write", op, value, insn))
            return f"WRITE({op.name}, {value})"

    @staticmethod
    def insn(name, sizes, prefix=()):
        ops = [SimpleNamespace(name=f"op{index}", size=size)
               for index, size in enumerate(sizes)]
        return SimpleNamespace(
            mnemonic=name, operands=ops, prefix=prefix, address=0x401000
        )

    def test_requested_host_hook_contract_and_width(self) -> None:
        host = self.Host()
        insn = self.insn("adc", (2, 2))
        lines = host.lift_flag_instruction(insn)
        self.assertEqual(
            lines,
            ["WRITE(op0, engine_flags_add(&eflags, op0, op1, "
             "(eflags & ENGINE_EFLAGS_CF), 16u));"],
        )
        self.assertTrue(all(call[-1] is insn for call in host.calls))

        self.assertEqual(
            host.lift_flag_instruction(self.insn("shl", (4, 1))),
            ["WRITE(op0, engine_flags_shl(&eflags, op0, op1, 32u));"],
        )
        self.assertEqual(
            host.lift_flag_instruction(self.insn("imul", (4, 4, 4))),
            ["WRITE(op0, engine_flags_imul(&eflags, op1, op2, 32u));"],
        )

    def test_aliases_and_fail_closed_errors(self) -> None:
        host = self.Host()
        self.assertEqual(host.flags_condition("setc"), host.flags_condition("jb"))
        self.assertEqual(host.flags_condition("cmovnz"), host.flags_condition("jne"))
        self.assertIsNone(host.lift_flag_instruction(self.insn("mov", (4, 4))))
        for mnemonic in ("shld", "div", "scasd", "xadd"):
            with self.subTest(rejected=mnemonic), self.assertRaises(NotImplementedError):
                host.lift_flag_instruction(self.insn(mnemonic, (4, 4)))
        with self.assertRaises(NotImplementedError):
            host.lift_flag_instruction(self.insn("imul", (4,)))
        with self.assertRaises(ValueError):
            host.lift_flag_instruction(self.insn("imul", (1, 1)))
        with self.assertRaises(ValueError):
            host.lift_flag_instruction(self.insn("add", (8, 8)))
        with self.assertRaises(ValueError):
            host.lift_flag_instruction(self.insn("add", (4, 2)))
        with self.assertRaises(ValueError):
            host.lift_flag_instruction(self.insn("add", (4, 4), (0xF0,)))
        with self.assertRaises(ValueError):
            host.lift_flag_instruction(self.insn("lock add", (4, 4)))
        with self.assertRaises(ValueError):
            host.flags_condition("jecxz")
        self.assertEqual(len(UNDEFINED_FLAG_NOTES), 6)
        print("RECEIPT FLAGS-003: mixin hook, width, alias, and rejection checks passed")


class FlagsSanitizerTests(unittest.TestCase):
    def test_header_has_no_undefined_behavior_on_valid_widths(self) -> None:
        compiler = shutil.which("clang")
        if compiler is None:
            self.skipTest("clang is required for the UBSan receipt")
        with tempfile.TemporaryDirectory(prefix="halo-flags-ubsan-") as temporary:
            directory = Path(temporary)
            source = directory / "flags_ubsan.c"
            executable = directory / "flags_ubsan"
            source.write_text(
                c_source() + '''
int main(void) {
    volatile uint64_t receipt = 0u;
    static const uint32_t values[] = {
        0u, 1u, 0x7fu, 0x80u, 0xffu, 0x7fffu, 0x8000u,
        0xffffu, 0x7fffffffu, 0x80000000u, 0xffffffffu
    };
    for (uint32_t op = 0u; op < 12u; ++op) {
        for (uint32_t width = 8u; width <= 32u; width *= 2u) {
            for (uint32_t i = 0u; i < sizeof(values) / sizeof(values[0]); ++i)
                receipt ^= flags_run(op, width, values[i], values[10u-i],
                                     ENGINE_EFLAGS_RESERVED1 | (i & 1u));
        }
    }
    for (uint32_t op = 0u; op < 3u; ++op) {
        for (uint32_t width = 8u; width <= 32u; width *= 2u) {
            for (uint32_t count = 0u; count < 64u; ++count)
                receipt ^= flags_shift_run(op, width, values[count % 11u],
                                           count, ENGINE_EFLAGS_RESERVED1);
        }
    }
    for (uint32_t width = 16u; width <= 32u; width *= 2u) {
        for (uint32_t i = 0u; i < sizeof(values) / sizeof(values[0]); ++i)
            receipt ^= flags_imul_run(width, values[i], values[10u-i],
                                      ENGINE_EFLAGS_RESERVED1);
    }
    return receipt == UINT64_MAX;
}
''',
                encoding="utf-8",
            )
            subprocess.run(
                [compiler, "-std=c11", "-O2", "-fsanitize=undefined",
                 "-fno-sanitize-recover=undefined", "-Wall", "-Wextra", "-Werror",
                 "-I", str(HEADER_DIR), str(source), "-o", str(executable)],
                check=True, capture_output=True, text=True,
            )
            subprocess.run([str(executable)], check=True, capture_output=True, text=True)
        print("RECEIPT FLAGS-004: UBSan valid-width sweep passed")


if __name__ == "__main__":
    unittest.main(verbosity=2)
