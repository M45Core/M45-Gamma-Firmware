#!/usr/bin/env python3
"""Check the real RX quiesce helpers against an in-flight receive thread."""
import pathlib
import re
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def function(source, name):
    match = re.search(r'^(?:static )?[\w *]+\b' + name + r'\([^;]*?\)\s*\{', source, re.M)
    if not match:
        raise RuntimeError(f'Cannot extract {name}')
    pos, depth = match.end(), 1
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[match.start():pos] + '\n'


source = (ROOT / 'main/stratum_minimal.c').read_text()
functions = '\n'.join(function(source, name) for name in [
    'stratum_minimal_wait_for_asic_rx_idle', 'read_asic_result'])
test = (ROOT / 'tests/asic_rx_quiesce_test.c').read_text()
with tempfile.TemporaryDirectory(prefix='m45-asic-rx-') as directory:
    out = pathlib.Path(directory)
    (out / 'test.c').write_text(test.replace('/* ASIC_RX_UNDER_TEST */', functions))
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-g', '-pthread', str(out / 'test.c'),
                    '-o', str(out / 'test')], check=True)
    subprocess.run([str(out / 'test')], check=True)
