#!/usr/bin/env python3
"""Guard against bypassing a hook when its address list changes.

Translated code calls fixed function entries directly unless engine_hooks.h
lists them, so every address a hook compares against and every override-table
entry must be listed there. Only the HALO_A10_TRACE hooks may sit in the trace
list, which goes direct in release builds."""
import pathlib, re
root = pathlib.Path(__file__).resolve().parents[1]
src = (root / 'overrides.c').read_text()
header = (root.parent / 'EngineReuse/engine_hooks.h').read_text()
def macro_list(name):
    body = header.split(f'#define {name}(X)')[1].split('\n\n')[0]
    return {int(x, 16) for x in re.findall(r'X\(0x([0-9a-fA-F]+)u\)', body)}
hooks, trace = macro_list('ENGINE_HOOK_ADDRESSES'), macro_list('ENGINE_TRACE_HOOK_ADDRESSES')
assert hooks and trace and not hooks & trace
files = ['a10_control.inc','a10_gamepad.inc','hsc_trace.inc','audio_ownership_trace.inc',
         'model_capture_hooks.inc','panorama_hooks.inc','panorama_overlay_scope.h','native_leaves.h','frame_pacing_hooks.inc']
required, trace_only = {0x004C6E80}, set()
for name in files:
    text = (root/name).read_text()
    macros = dict(re.findall(r'#define\s+(\w+)\s+(0x[0-9a-fA-F]+)u?',text))
    for token in re.findall(r'\baddress\s*[!=]=\s*(0x[0-9a-fA-F]+u?|[A-Z_]+)',text):
        value = int(macros.get(token,token).rstrip('uU'), 16)
        (trace_only if name == 'a10_control.inc' else required).add(value)
dispatch_body = src.split('int engine_dispatch_override(EngineCPU *cpu, uint32_t address) {')[1]
required |= {int(x, 16) for x in re.findall(r'\baddress == 0x([0-9a-fA-F]+)u', dispatch_body)}
trace_only -= required | {0x004C6E80}
missing = (required | trace_only) - hooks - trace
assert not missing, 'Hook addresses missing from engine_hooks.h: '+str([hex(x) for x in sorted(missing)])
wrongly_trace = required & trace
assert not wrongly_trace, 'Behavioural hooks in the trace list, which goes direct: '+str([hex(x) for x in sorted(wrongly_trace)])
table = src.split('static Override overrides[] = {')[1].split('};')[0]
overridden = {int(x, 16) for x in re.findall(r'\{\s*0x([0-9a-fA-F]+)u', table)} - {0xFFFFFFFF}
assert overridden and not overridden - hooks, 'Override table entries missing from engine_hooks.h: '+str([hex(x) for x in sorted(overridden - hooks)])
assert 'ENGINE_HOOK_ADDRESSES(DISPATCH_HOOK_ADDRESS)' in src and 'ENGINE_TRACE_HOOK_ADDRESSES(DISPATCH_HOOK_ADDRESS)' in src
assert 'dispatch_interest_add(overrides[i].addr)' in src
print(f'engine_hooks.h routes all {len(required)} hook addresses and {len(overridden)} overrides; '
      f'{len(trace_only)} trace-only: PASS')
