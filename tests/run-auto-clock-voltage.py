#!/usr/bin/env python3
"""Exercise the real controller on a host, with only device I/O stubbed."""
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


hw = (ROOT / 'main/bitaxe_hw.c').read_text()
cfg = (ROOT / 'main/m45_config.c').read_text()
code = (ROOT / 'tests/auto_clock_voltage_test.c').read_text()
helpers = '\n'.join(line for line in hw.splitlines() if line.startswith('#define AUTO_CLOCK_')) + '\n'
helpers += '\n'.join(line for line in cfg.splitlines() if re.match(r'#define M45_ASIC_(TEMP_COMP_|VOLTAGE_(MIN|MAX)_MV)', line)) + '\n'
status = function(hw, 'update_auto_clock_status')
helpers += status[:status.index('{')] + '''{
    g_auto_clock_status.input_voltage_limited = input_voltage_limited;
    snprintf(g_auto_clock_status.hold_reason, sizeof(g_auto_clock_status.hold_reason), "%s", hold_reason);
}\n'''
for name in ['m45_config_effective_asic_frequency_mhz', 'm45_config_effective_asic_voltage_mv',
             'm45_config_asic_voltage_temp_compensation_mv', 'm45_config_effective_asic_voltage_mv_for_temp',
             'm45_config_effective_auto_clock_target_temp_c', 'm45_config_effective_auto_clock_max_watts']:
    helpers += function(cfg, name)
for name in ['auto_clock_preset_frequency', 'auto_clock_preset_count', 'auto_clock_index_for_frequency',
             'auto_clock_base_voltage_mv', 'auto_clock_candidate_voltage', 'current_auto_clock_frequency_mhz',
             'current_auto_clock_voltage_mv', 'auto_clock_upshift_current_ceiling_a',
             'auto_clock_upshift_vr_temp_ceiling_c', 'auto_clock_clamp_toward', 'apply_auto_clock_control']:
    helpers += function(hw, name)
with tempfile.TemporaryDirectory(prefix='m45-auto-clock-') as directory:
    out = pathlib.Path(directory)
    (out / 'test.c').write_text(code.replace('/* CONTROLLER_UNDER_TEST */', helpers))
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                    '-fsanitize=address,undefined', '-g', '-I' + str(ROOT / 'tests/diagnostic_stubs'),
                    '-I' + str(ROOT / 'main'), str(out / 'test.c'),
                    str(ROOT / 'main/m45_auto_clock_voltage.c'), '-lm', '-o', str(out / 'test')], check=True)
    subprocess.run([str(out / 'test')], check=True)
