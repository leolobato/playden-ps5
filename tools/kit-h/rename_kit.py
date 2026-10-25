#!/usr/bin/env python3
# Gives a staged kit another id: its folder on the console (/data/playden/kits/<id>), the manifest's kit_id and the
# zip's inner folder. Kit 0.13 is the first kit installed as playden-kit. The template bottle's user.reg points Wine
# Mono at the kit folder (HKCU\Software\Wine\Mono RuntimePath, see update_r4.py), so that value follows; prospero-win's
# packager refuses a template whose RuntimePath names another folder. Bottles copied from the earlier template still
# name the earlier folder: Playden points them at the new one (PointBottleAtKitMono), so they need no Recreate.
# Run on the Mac on a staged kit (kit-spec.json and inputs/), before packaging:
#   tools/kit-h/rename_kit.py build/kit-<version> playden-kit
import json, re, sys
from pathlib import Path

here, kit_id = Path(sys.argv[1]).resolve(), sys.argv[2]
assert re.fullmatch(r'[a-z0-9][a-z0-9._-]*', kit_id), f'not a kit id: {kit_id}'
spec_path = here / 'kit-spec.json'
spec = json.loads(spec_path.read_text())
previous = spec['kit_id']
assert previous != kit_id, f'the kit is already {kit_id}'
user_reg = Path(spec['template']['path'])
user_reg = (user_reg if user_reg.is_absolute() else here / user_reg) / 'user.reg'
registry = user_reg.read_text()
old = '"RuntimePath"="Z:\\\\data\\\\playden\\\\kits\\\\' + previous + '\\\\'
new = '"RuntimePath"="Z:\\\\data\\\\playden\\\\kits\\\\' + kit_id + '\\\\'
if 'wine-mono' in spec['capabilities']:
    assert registry.count(old) == 1, f'{user_reg}: no single Wine Mono RuntimePath under {previous}'
    user_reg.write_text(registry.replace(old, new))
spec['kit_id'] = kit_id
spec_path.write_text(json.dumps(spec, indent=1, sort_keys=True) + '\n')
record_path = here / 'inputs/stage-record.json'
if record_path.exists():
    record = json.loads(record_path.read_text())
    record['kit_id'] = {'from': previous, 'to': kit_id, 'template_changed': ['user.reg']}
    record_path.write_text(json.dumps(record, indent=1) + '\n')
print(f'kit {previous} -> {kit_id}' + (' (template RuntimePath too)' if 'wine-mono' in spec['capabilities'] else ''))
