#!/usr/bin/env python3
"""Static fail-closed configuration/JSON consumer checks; never execute FreeRDP."""
import json
from pathlib import Path
import re
import subprocess
import sys


def check(build, features, elf=None):
    cache = (build / 'CMakeCache.txt').read_text().splitlines()
    actual = sorted(x for x in cache if x.startswith(('WITH_', 'CHANNEL_')) and ':BOOL=' in x)
    if actual != features.read_text().splitlines():
        raise ValueError('WITH/CHANNEL feature drift')
    for required in ('BUILD_SHARED_LIBS:BOOL=ON', 'USE_VERSION_FROM_GIT_TAG:BOOL=OFF',
                     'USE_GIT_FOR_REVISION:BOOL=OFF',
                     'CMAKE_INSTALL_PREFIX:PATH=/usr/lib/omawin365/freerdp'):
        if cache.count(required) != 1:
            raise ValueError('missing/ambiguous invariant: ' + required)
    # No such option exists in this pinned source; do not accept an ignored override.
    if any(x.startswith('BUILTIN_CHANNELS:') for x in cache):
        raise ValueError('unsupported BUILTIN_CHANNELS override')
    version = (build / 'include/freerdp/version.h').read_text()
    for line in ('#define FREERDP_VERSION_FULL "3.32.1"', '#define FREERDP_GIT_REVISION "n/a"'):
        if version.splitlines().count(line) != 1:
            raise ValueError('unexpected generated version: ' + line)
    entries = json.loads((build / 'compile_commands.json').read_text())
    implementations = [x for x in entries if '/utils/json/' in x['file']
                       and Path(x['file']).name != 'json.c']
    if len(implementations) != 1 or Path(implementations[0]['file']).name != 'jansson.c':
        raise ValueError('JSON implementation changed')
    command = implementations[0].get('command', ' '.join(implementations[0].get('arguments', [])))
    if not re.search(r'(^|\s)-DWITH_JANSSON(?:=\S+)?(?=\s|$)', command):
        raise ValueError('missing generated WITH_JANSSON definition')
    if elf is not None:
        dynamic = subprocess.check_output(['readelf', '-d', str(elf)], text=True)
        needed = re.findall(r'Shared library: \[([^]]+)\]', dynamic)
        if needed.count('libjansson.so.4') != 1 or any('json-c' in x or 'cjson' in x.lower() for x in needed):
            raise ValueError('JSON ELF dependency changed')
    print('PASS configuration/version/generated jansson selection' + ('/ELF' if elf else ''))


if __name__ == '__main__':
    try:
        if len(sys.argv) not in (3, 4):
            raise ValueError('usage: check-contract.py BUILD FEATURES [LIBWINPR_ELF]')
        check(Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]) if len(sys.argv) == 4 else None)
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        print('FAIL: ' + str(exc), file=sys.stderr)
        sys.exit(1)
