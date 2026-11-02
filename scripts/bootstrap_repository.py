#!/usr/bin/env python3
"""Restore the exact gitlinks when using the source ZIP instead of the Git bundle.
Run once in an extracted, trusted source snapshot. Does not fetch or push anything.
"""
import json
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
if (root / '.git').exists():
    raise SystemExit('This directory is already a Git repository; no changes made.')

def git(*args):
    subprocess.run(['git', *args], cwd=root, check=True)

lock = json.loads((root / 'plugins-upstream.lock.json').read_text())
git('init', '-b', 'main')
git('add', '--all')
# ZIP extraction on Windows does not preserve Unix executable bits.
for path in lock.get('executable_files', []):
    git('update-index', '--chmod=+x', '--', path)
for module in lock['submodules']:
    git('update-index', '--add', '--cacheinfo', f"160000,{module['revision']},{module['path']}")
git('-c', 'user.name=AyuGram Integration Snapshot',
    '-c', 'user.email=integration@example.invalid',
    'commit', '-m', f"AyuGram plugin integration, based on {lock['revision']}")
print('Repository initialized with pinned submodules. Add your own remote, then push main.')
print('For a local build: git submodule update --init --recursive')
