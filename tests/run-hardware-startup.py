#!/usr/bin/env python3
"""Run the firmware's real single-start control flow with device I/O stubbed."""
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


source = (ROOT / 'main/bitaxe_hw.c').read_text()
functions = '\n'.join(function(source, name) for name in [
    'bitaxe_gamma602_log_read_retry', 'shutdown_and_return', 'initialize_hardware_unlocked',
    'bitaxe_gamma602_start_hardware_unlocked',
    'bitaxe_gamma602_set_asic_power'])
test = (ROOT / 'tests/hardware_startup_test.c').read_text()
grace = re.search(r'^#define ASIC_RX_DRAIN_TIMEOUT_MS \d+$', source, re.M)
if grace is None:
    raise RuntimeError('Missing ASIC RX drain timeout')
test = test.replace('/* RX_DRAIN_TIMEOUT_UNDER_TEST */', grace.group(0))
with tempfile.TemporaryDirectory(prefix='m45-hardware-startup-') as directory:
    out = pathlib.Path(directory)
    (out / 'test.c').write_text(test.replace('/* HARDWARE_UNDER_TEST */', functions))
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-g', str(out / 'test.c'),
                    '-o', str(out / 'test')], check=True)
    subprocess.run([str(out / 'test')], check=True)
