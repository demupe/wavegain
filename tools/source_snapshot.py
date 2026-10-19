"""Source snapshots for experiments spanning the old root and src/ layouts."""
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'src'


def revision_sources(ref):
    probe = subprocess.run(['git', 'cat-file', '-e', f'{ref}:src/gain_analysis.c'],
                           cwd=ROOT, capture_output=True)
    prefix = 'src/' if probe.returncode == 0 else ''
    tree = f'{ref}:src' if prefix else ref
    names = subprocess.check_output(['git', 'ls-tree', '--name-only', tree],
                                    cwd=ROOT, text=True).splitlines()
    return {name: subprocess.check_output(['git', 'show', f'{ref}:{prefix}{name}'], cwd=ROOT)
            for name in names if name.endswith(('.c', '.h'))}


def working_sources():
    return {p.name: p.read_bytes() for p in [*SRC.glob('*.c'), *SRC.glob('*.h')]}


def write_flat_sources(folder, sources, ref=None):
    """Keep historical string-based experiments flat, including vendor hooks."""
    for name, data in sources.items():
        if name == 'loudness.c':
            data = data.replace(b'#include "../vendor/', b'#include "vendor/')
        (folder / name).write_bytes(data)
    if 'loudness.c' in sources:
        if ref is None:
            shutil.copytree(ROOT / 'vendor', folder / 'vendor', dirs_exist_ok=True)
        else:
            names = subprocess.check_output(['git', 'ls-tree', '-r', '--name-only', ref, '--', 'vendor'],
                                            cwd=ROOT, text=True).splitlines()
            for name in names:
                path = folder / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(subprocess.check_output(['git', 'show', f'{ref}:{name}'], cwd=ROOT))
        hook = folder / 'vendor/libebur128/ebur128.c'
        hook.write_bytes(hook.read_bytes().replace(b'"../../src/loudness_fast.h"',
                                                  b'"../../loudness_fast.h"'))
