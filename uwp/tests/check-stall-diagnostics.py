#!/usr/bin/env python3
"""Compile actual diagnostic header and extracted scheduler method; no Xbox performance claim."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
cxx = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++')
if not cxx:
    raise SystemExit('C++ compiler missing')

source = (root / 'rpcs3/Emu/Cell/lv2/lv2.cpp').read_text()
start = source.index('std::string lv2_obj::uwp_scheduler_snapshot()')
opening = source.index('{', start)
depth = 1
end = opening + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
method = source[start:end]

with tempfile.TemporaryDirectory(prefix='rpcs3-stall-check-') as directory:
    work = Path(directory)
    template = (root / 'uwp/tests/scheduler-snapshot-harness.cpp').read_text()
    scheduler = work / 'scheduler.cpp'
    scheduler.write_text(template.replace('// PRODUCTION_SCHEDULER_SNAPSHOT', method))
    for name, path in [('rsx', root / 'uwp/tests/stall-diagnostics-harness.cpp'), ('scheduler', scheduler)]:
        binary = work / name
        subprocess.run([cxx, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
                        '-I', str(root / 'rpcs3'), str(path), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True, timeout=30)
print('Nonblocking scheduler snapshot checks passed (host only)')
