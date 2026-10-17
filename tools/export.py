#!/usr/bin/env python3
"""Export only reviewed allowlisted sources; never includes parent Git history."""
import argparse
import hashlib
import io
from pathlib import Path
import tarfile

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--output', type=Path, default=ROOT / 'dist/mistlane-0.1.0.tar.gz')
args = p.parse_args()
files = (ROOT / 'release-files.txt').read_text().splitlines()
if not files or len(files) != len(set(files)):
    raise ValueError('invalid release allowlist')
manifest = []
for name in files:
    path = ROOT / name
    if Path(name).is_absolute() or '..' in Path(name).parts:
        raise ValueError(f'invalid release path: {name}')
    if not path.is_file() or path.is_symlink() or not path.resolve().is_relative_to(ROOT):
        raise ValueError(f'missing or unsafe source file: {name}')
    manifest.append(f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {name}')
if args.output.resolve() in {(ROOT / name).resolve() for name in files}:
    raise ValueError('release output would overwrite a source file')
args.output.parent.mkdir(parents=True, exist_ok=True)
# No recursive add: unlisted private/config/generated files cannot slip into a release.
with tarfile.open(args.output, 'w:gz') as tar:
    for name in files:
        tar.add(ROOT / name, arcname='mistlane/' + name, recursive=False)
    data = ('\n'.join(manifest)+'\n').encode()
    info = tarfile.TarInfo('mistlane/SHA256SUMS')
    info.size = len(data)
    tar.addfile(info, io.BytesIO(data))
print(f'{args.output}: {len(files)} source files; SHA256SUMS included')
