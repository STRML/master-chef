#!/usr/bin/env python3
"""Exercise the real runner's reporting tail without compiling/launching Halo."""
import ast
import json
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
runner = root / 'tools/probe_engine_menu_input.py'
tree = ast.parse(runner.read_text())
start = next(i for i, node in enumerate(tree.body)
             if isinstance(node, ast.Assign)
             and any(isinstance(target, ast.Name) and target.id == 'receipt'
                     for target in node.targets))
# Execute the same receipt/log/exit statements, with controlled child outcomes.
tail = ast.unparse(ast.Module(body=tree.body[start:], type_ignores=[]))
prefix = '''import json, pathlib, time
from types import SimpleNamespace
out=pathlib.Path('.')
a=SimpleNamespace(mode='neutral',timeout=1)
game_source=game=out
env={}
started=time.time()
'''
with tempfile.TemporaryDirectory(prefix='halo-probe-exit-') as directory:
    out = Path(directory)
    (out / 'host.log').write_text('diagnostic fixture\n')
    (out / 'frame-0001.bgra').write_bytes(b'fixture')
    for child, expected in [(0, 0), (7, 7), (124, 124), (-11, 139)]:
        result = subprocess.run([sys.executable, '-c', prefix + f'code={child}\n' + tail],
                                cwd=out, capture_output=True, text=True)
        assert result.returncode == expected, (child, result.returncode, result.stderr)
        receipt = json.loads((out / 'run.json').read_text())
        assert receipt['exitCode'] == child
        assert receipt['capturedFrames'] == ['frame-0001.bgra']
        assert 'diagnostic fixture' in result.stdout
        assert (out / 'frame-0001.bgra').read_bytes() == b'fixture'
        print(f'PASS child={child} shell={expected}; receipt and evidence preserved')
print('Reporting-only fixtures passed; no game or GPU execution performed.')
