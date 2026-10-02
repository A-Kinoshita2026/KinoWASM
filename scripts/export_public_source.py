#!/usr/bin/env python3
"""Copy the public source distribution to a new directory without Git history."""

import argparse
import hashlib
from pathlib import Path
import shutil

ROOT_FILES = (
    '.gitignore', 'CMakeLists.txt', 'CMakePresets.json',
    'LICENSE', 'README.md', 'README.ja.md',
)
SOURCE_DIRS = ('KinoWASM', 'apps', 'host', 'Test', 'WASMData', 'docs', 'tools', 'examples')
SOURCE_EXTENSIONS = {
    '.c', '.h', '.cpp', '.hpp', '.inc', '.md', '.txt', '.json',
    '.py', '.bat', '.wat', '.wast', '.tsv', '.hint', '.def', '.cs',
}
EXTRA_FILES = (
    'WASMData/library.wasm', 'WASMData/start.wasm',
    'Test/wasi/wasi_fs_data/digits.bin',
    'scripts/pgo_build.bat', 'scripts/export_public_source.py',
)


def select_files(root):
    selected = set(ROOT_FILES) | set(EXTRA_FILES)
    for directory in SOURCE_DIRS:
        for path in (root / directory).rglob('*'):
            if path.is_symlink():
                raise ValueError(f'Symlinks are not supported: {path}')
            if not path.is_file():
                continue
            relative = path.relative_to(root)
            if any(part.startswith('.') or part == '__pycache__' for part in relative.parts):
                continue
            if path.suffix.lower() in SOURCE_EXTENSIONS:
                selected.add(relative.as_posix())
    for name in sorted(selected):
        source = root / name
        if not source.is_file() or source.is_symlink():
            raise ValueError(f'Missing or invalid distribution input: {name}')
        yield name


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', type=Path, help='New, nonexistent output directory')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    destination = args.destination.resolve()
    if destination.exists():
        parser.error('Destination already exists; choose a new directory.')
    if root == destination or destination in root.parents:
        parser.error('Destination must not be the source root or one of its parents.')
    for directory in SOURCE_DIRS:
        source_dir = root / directory
        if destination == source_dir or source_dir in destination.parents:
            parser.error('Destination must not be inside a public source directory.')
    names = list(select_files(root))
    destination.mkdir(parents=True)
    for name in names:
        source = root / name
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        if hashlib.sha256(source.read_bytes()).digest() != hashlib.sha256(target.read_bytes()).digest():
            raise RuntimeError(f'Copy verification failed: {name}')
    print(f'Copied and verified {len(names)} files to {destination}')
    print('Excluded: Git history, Archive, Materials, Bin, build outputs, agent settings, and legacy build script.')
    print('This selection is a source packaging step, not a license or secret audit.')


if __name__ == '__main__':
    main()
