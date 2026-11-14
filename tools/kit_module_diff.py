#!/usr/bin/env python3
# Playden - Compare a Wine cross build's modules with a runtime kit's.
# Copyright (C) 2026 Leo Lobato
# SPDX-License-Identifier: GPL-3.0-or-later
"""List the modules of a prospero-win cross build that differ from a runtime kit's.

A development kit takes an earlier kit and swaps in the modules a change touched. This tool finds them:

    python3 tools/kit_module_diff.py <work folder> <kit's wine/lib/wine folder>

The work folder is what tools/build_wine_ps5.sh wrote (prx/sce_module/*.prx and pe/<arch>-windows/*). The kit
folder holds x86_64-unix/*.prx and <arch>-windows/*, for example <kit inputs>/runtime/wine/lib/wine.

A .prx is compared byte for byte. A kit carries its PE modules stripped (strip -s), and the cross build leaves them
unstripped, so each PE module and the kit's copy are both stripped into temporary copies with the MinGW strip of its
architecture first. strip writes the current time into a PE file unless SOURCE_DATE_EPOCH is set, so both are stripped
with the same SOURCE_DATE_EPOCH: otherwise every PE module would differ, even one the build didn't change, and the
kit's copy carries the time it was stripped. Builds from before prospero-win made its PE output reproducible carry the link time and a work-folder-dependent
CodeView GUID, so every PE module differs from them; compare with a kit built after that change.

--json prints the result as JSON. The exit status is 0 whether or not modules differ, 2 on a usage error.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ARCHES = {'i386': 'i686-w64-mingw32-strip', 'x86_64': 'x86_64-w64-mingw32-strip'}
# Any fixed time: both sides are stripped with it, so only their code can differ.
STRIP_EPOCH = '1'


def stripped(source: Path, copy: Path, strip: str) -> bytes:
    shutil.copyfile(source, copy)
    subprocess.run([strip, '-s', str(copy)], check=True, env=dict(os.environ, SOURCE_DATE_EPOCH=STRIP_EPOCH))
    return copy.read_bytes()


def compare(work: Path, kit: Path, strip_commands: dict[str, str]) -> dict[str, list[str]]:
    result: dict[str, list[str]] = {'changed': [], 'same': [], 'new': []}
    pairs = [(path, kit / 'x86_64-unix' / path.name, f'x86_64-unix/{path.name}', None)
             for path in sorted((work / 'prx/sce_module').glob('*.prx'))]
    for arch in ARCHES:
        pairs += [(path, kit / f'{arch}-windows' / path.name, f'{arch}-windows/{path.name}', arch)
                  for path in sorted((work / f'pe/{arch}-windows').glob('*')) if path.is_file()]
    with tempfile.TemporaryDirectory() as directory:
        for built, shipped, name, arch in pairs:
            if not shipped.is_file():
                result['new'].append(name)
                continue
            if arch is None:
                data, kit_data = built.read_bytes(), shipped.read_bytes()
            else:
                data = stripped(built, Path(directory) / f'built-{arch}-{built.name}', strip_commands[arch])
                kit_data = stripped(shipped, Path(directory) / f'kit-{arch}-{built.name}', strip_commands[arch])
            result['same' if data == kit_data else 'changed'].append(name)
    return result


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('work', type=Path, help='the cross build work folder')
    parser.add_argument('kit', type=Path, help="the kit's wine/lib/wine folder")
    parser.add_argument('--json', action='store_true', help='print the result as JSON')
    for arch, command in ARCHES.items():
        parser.add_argument(f'--strip-{arch}', default=command, help=f'the strip command for {arch} (default {command})')
    args = parser.parse_args(argv)
    if not (args.work / 'prx/sce_module').is_dir() and not (args.work / 'pe').is_dir():
        parser.error(f'{args.work} has no prx/sce_module or pe folder')
    if not args.kit.is_dir():
        parser.error(f'{args.kit} is not a folder')
    result = compare(args.work, args.kit, {arch: getattr(args, f'strip_{arch}') for arch in ARCHES})
    if args.json:
        print(json.dumps(result, indent=1))
    else:
        for name in result['changed']:
            print(f'changed {name}')
        for name in result['new']:
            print(f'new     {name}')
        print(f"{len(result['changed'])} changed, {len(result['new'])} new, {len(result['same'])} the same")
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
