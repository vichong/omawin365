#!/usr/bin/env python3
"""Generate a LOCAL checksummed HEAD snapshot recipe, outside the input checkout."""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys


def main():
    if len(sys.argv) != 3:
        raise ValueError('usage: prepare-snapshot.py CHECKOUT NEW-OUTSIDE-DIRECTORY')
    checkout = Path(sys.argv[1]).resolve(strict=True)
    out = Path(sys.argv[2]).resolve()
    env = dict(os.environ, GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL='/dev/null')
    def git(*args):
        return subprocess.check_output(['git', '-C', str(checkout), *args], env=env)
    root = Path(git('rev-parse', '--show-toplevel').decode().strip()).resolve()
    if checkout != root or out.is_relative_to(root):
        raise ValueError('require checkout root and output outside it')
    commit = git('rev-parse', '--verify', 'HEAD^{commit}').decode().strip()
    if not re.fullmatch(r'[a-f0-9]{40}|[a-f0-9]{64}', commit):
        raise ValueError('invalid committed HEAD identity')
    # Select a real application commit, not an unrelated repository.
    git('cat-file', '-e', commit + ':omawin365.pro')
    git('cat-file', '-e', commit + ':src/session.cpp')
    template_path = 'packaging/app/PKGBUILD.in'
    launcher_path = 'packaging/app/omawin365-launcher'
    template_bytes = git('cat-file', 'blob', commit + ':' + template_path)
    launcher = git('cat-file', 'blob', commit + ':' + launcher_path)
    template = template_bytes.decode('utf-8')
    template_digest = hashlib.sha256(template_bytes).hexdigest()
    # Exclusive creation: no overwriting existing output or user artifacts.
    out.mkdir(mode=0o700)
    archive = out / 'omawin365-source.tar'
    with archive.open('xb') as sink:
        subprocess.run(['git', '-C', str(root), 'archive', '--format=tar',
                        '--prefix=omawin365/', commit], env=env, stdout=sink, check=True)
    with archive.open('rb') as source:
        digest = hashlib.file_digest(source, 'sha256').hexdigest()
    launcher_digest = hashlib.sha256(launcher).hexdigest()
    values = {'@PKGVER@': '0.0.0.local' + commit[:16], '@ARCHIVE_SHA256@': digest,
              '@LAUNCHER_SHA256@': launcher_digest}
    for key, value in values.items():
        if template.count(key) != 1:
            raise ValueError('ambiguous template field: ' + key)
        template = template.replace(key, value)
    if re.search(r'@[A-Z_]+@', template):
        raise ValueError('unresolved template field')
    (out / 'PKGBUILD').write_text(template)
    (out / 'omawin365-launcher').write_bytes(launcher)
    (out / 'snapshot.json').write_text(json.dumps({'kind': 'local committed HEAD snapshot',
        'commit': commit, 'archive_sha256': digest, 'launcher_sha256': launcher_digest,
        'packaging_inputs': {'origin_commit': commit,
            'template': {'path': template_path, 'sha256': template_digest},
            'launcher': {'path': launcher_path, 'sha256': launcher_digest}}}, indent=2) + '\n')
    for p in out.iterdir():
        p.chmod(0o600)
    print('Prepared local committed snapshot ' + commit + '; uncommitted edits excluded.')
    print('No public release URL/tag/version was selected.')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        print('FAIL: ' + str(exc), file=sys.stderr)
        sys.exit(1)
