#!/usr/bin/env python3
import configparser
import pathlib
import re
import subprocess
import sys
from typing import Dict

ROOT = pathlib.Path(__file__).resolve().parents[2]
GITMODULES = ROOT / '.gitmodules'
LOCK = ROOT / 'cpp' / 'submodules.lock'

REQUIRED_PATHS = {
    'cpp/third_party/usearch',
    'cpp/third_party/sqlite',
    'cpp/third_party/googletest',
    'cpp/third_party/libtorch-dist',
}


def fail(msg: str) -> None:
    print(f'[verify_submodules] ERROR: {msg}', file=sys.stderr)
    sys.exit(1)


def warn(msg: str) -> None:
    print(f'[verify_submodules] WARNING: {msg}')


def parse_lock_file(text: str) -> Dict[str, str]:
    pinned_by_path: Dict[str, str] = {}
    current_path = None
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if line.startswith('path:'):
            current_path = line.split(':', 1)[1].strip()
            continue
        if line.startswith('pinned_commit:'):
            if current_path is None:
                continue
            pinned = line.split(':', 1)[1].strip().strip('"')
            pinned_by_path[current_path] = pinned
    return pinned_by_path


def parse_submodule_status() -> Dict[str, str]:
    cmd = ['git', 'submodule', 'status', '--recursive']
    result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, check=True)
    status: Dict[str, str] = {}
    for raw_line in result.stdout.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        # Format: "<prefix><sha> <path> ..."
        match = re.match(r'^[\-\+U ]?([0-9a-f]{40})\s+([^\s]+)', line)
        if not match:
            continue
        sha = match.group(1)
        path = match.group(2)
        status[path] = sha
    return status


if not GITMODULES.exists():
    fail('.gitmodules file is missing')

if not LOCK.exists():
    fail('cpp/submodules.lock is missing')

parser = configparser.ConfigParser()
parser.read(GITMODULES, encoding='utf-8')

paths = set()
for section in parser.sections():
    if not section.startswith('submodule '):
        continue
    path = parser.get(section, 'path', fallback='').strip()
    url = parser.get(section, 'url', fallback='').strip()
    if not path:
        fail(f'{section} missing path')
    if not url:
        fail(f'{section} missing url')
    paths.add(path)

missing = REQUIRED_PATHS - paths
if missing:
    fail(f'missing required submodule paths: {sorted(missing)}')

lock_text = LOCK.read_text(encoding='utf-8')
for path in sorted(REQUIRED_PATHS):
    if path not in lock_text:
        fail(f'lock file does not mention {path}')

pinned_by_path = parse_lock_file(lock_text)
for path in sorted(REQUIRED_PATHS):
    if path not in pinned_by_path:
        fail(f'lock file missing pinned_commit for {path}')

if any(v == '<PIN_REQUIRED>' for v in pinned_by_path.values()):
    warn('pinned commits are placeholders and must be updated before release')

status = parse_submodule_status()
if not status:
    warn('no initialized submodule gitlinks found in index yet')
else:
    for path in sorted(REQUIRED_PATHS):
        if path not in status:
            warn(f'{path} is declared but not initialized')
            continue
        pinned = pinned_by_path.get(path, '<PIN_REQUIRED>')
        if pinned != '<PIN_REQUIRED>' and pinned != status[path]:
            fail(f'commit mismatch for {path}: expected {pinned}, got {status[path]}')

print('[verify_submodules] OK: submodule policy files are consistent')
