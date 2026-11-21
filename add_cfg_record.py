"""Record the seed and skip-setup wiring in configuration.

The record checker pairs each touched path with the area that owns it. .github/workflows/build.yml and
board.toml both belong to configuration, so this change needs a configuration finding even though the
code it runs (make-data-seed.py, seed-data-skip-setup.py) belongs to storage.
"""
import pathlib, re, tomllib, json, sys

sys.stdout.reconfigure(encoding='utf-8')
p = pathlib.Path('porting.toml')
raw = p.read_text(encoding='utf-8')

EVIDENCE_PATH = 'evidence/2026-11-21-os-volume-and-data-seed.md'
REV = '29c43df20518e888ddb0ef38a0d9424f5ec93138'


def ts(s):
    assert '\\' not in s, s
    return json.dumps(s, ensure_ascii=False)


starts = [m.start() for m in re.finditer(r'^\[\[area\]\]$', raw, re.M)] + [len(raw)]
bounds = None
for i in range(len(starts) - 1):
    seg = raw[starts[i]:starts[i + 1]]
    if re.search(r'^id = "configuration"$', seg, re.M):
        bounds = (starts[i], starts[i + 1])
        break
assert bounds
base, end = bounds
lines = raw[base:end].split('\n')

ev_line = next(i for i, l in enumerate(lines) if l.startswith('evidence = ['))
ev_close = next(i for i in range(ev_line, len(lines)) if lines[i].rstrip() == ']')
rows = [l.rstrip().rstrip(',') + ',' for l in lines[ev_line + 1:ev_close] if l.strip()]
row = '  { kind = "source", reference = ' + ts('.github/workflows/build.yml') + ', result = ' + ts(
    'the Make the Data seed step and the payload step now pass --skip-setup, and board.toml names '
    'skip_setup_tool; the seed step failed once on the runner with hdiutil attach image not recognized '
    'before the two-step create/attach/newfs_apfs sequence replaced the single -fs create'
) + ' }'
lines = lines[:ev_line + 1] + rows + [row] + lines[ev_close:]

findings = [
    'A Data seed without Setup preferences reaches the Hello pages, not the home screen (2026-11-21): seed-data-skip-setup.py writes the purplebuddy, springboard and migration preferences (SetupDone, SetupFinishedAllSteps, SetupVersion 11, BKNoWatchdogs, LastSystemVersion) and creates the /private/var/containers directories that live on the data volume. board.toml therefore names it as [payload] skip_setup_tool and the payload step passes --skip-setup, because an empty container alone boots into Setup.',
    'hdiutil create -fs APFS and the following attach do not agree (2026-11-21): the first seed step failed on the runner with hdiutil attach failed - image not recognized for a -type SPARSE -fs APFS image. The working sequence is two steps: create an empty UDRW UDIF image, attach it without -mount, run newfs_apfs on the device, then dd the raw character device. run() echoes each command, so a later failure names the invocation instead of reporting only that hdiutil failed.',
]
fl = next(i for i, l in enumerate(lines) if l.startswith('findings = ['))
assert lines[fl].rstrip().endswith(']')
lines[fl] = lines[fl].rstrip()[:-1] + ''.join(', ' + ts(f) for f in findings) + ']'

out = raw[:base] + '\n'.join(lines) + raw[end:]

try:
    check = tomllib.loads(out)
except tomllib.TOMLDecodeError as e:
    kept = out.split('\n')
    n = int(str(e).split('line ')[1].split(',')[0])
    print('parse failed at line', n)
    for i in range(max(0, n - 3), min(len(kept), n + 2)):
        print(('>>> ' if i == n - 1 else '    ') + kept[i][:220])
    raise SystemExit(1)

cf = [x for x in check['area'] if x['id'] == 'configuration'][0]
print('configuration findings', len(cf['findings']), 'evidence', len(cf['evidence']))
p.write_text(out, encoding='utf-8')
