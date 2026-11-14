"""Record a freshly built kernel against the actual XNU overlay inputs (offline)."""
from __future__ import annotations
import argparse
import re
import hashlib
import json
import struct
from pathlib import Path
REVISION = b'K60PRO_STATIC_FIX_V1'
SOURCE_PREFIX = b'K60PRO_SOURCE_SHA256:'
DERIVED_HEADER = 'xnu/files/bsd/sys/redwood_build_provenance.h'

def source_digest(repo):
    repo = Path(repo)
    paths = []
    for directory in ('xnu/files', 'xnu/patches', 'xnu26'):
        base = repo / directory
        if not base.is_dir():
            raise ValueError('missing kernel input directory: ' + directory)
        paths.extend(p for p in base.rglob('*') if p.is_file() and '__pycache__' not in p.parts)
    digest = hashlib.sha256()
    for path in sorted(paths, key=lambda p: p.relative_to(repo).as_posix()):
        relative = path.relative_to(repo).as_posix()
        if relative == DERIVED_HEADER:
            continue
        digest.update(relative.encode() + b'\0')
        data = path.read_bytes()
        # Compiler inputs are text; Git's platform newline conversion must
        # not invent a different source revision between macOS and Windows.
        try:
            data = data.decode('utf-8').replace('\r\n', '\n').encode('utf-8')
        except UnicodeDecodeError:
            pass
        digest.update(hashlib.sha256(data).digest())
    return digest.hexdigest()

def kernel_record(kernel, repo):
    data = Path(kernel).read_bytes()
    if len(data) < 32 or struct.unpack_from('<4I', data) != (0xFEEDFACF, 0x0100000C, 0, 2):
        raise ValueError('kernel must be an ARM64 Mach-O executable')
    if REVISION not in data:
        raise ValueError('kernel needs rebuilding with K60PRO_STATIC_FIX_V1')
    stamps = re.findall(re.escape(SOURCE_PREFIX) + rb'([0-9a-f]{64})\x00', data)
    if len(stamps) != 1 or data.count(SOURCE_PREFIX) != 1:
        raise ValueError('kernel needs rebuilding with one embedded K60PRO_SOURCE_SHA256 digest')
    embedded = stamps[0].decode('ascii')
    current = source_digest(repo)
    if embedded != current:
        raise ValueError('kernel embedded source digest does not match current source inputs; rebuild')
    return {'schema_version': 1, 'revision': REVISION.decode(),
            'kernel_sha256': hashlib.sha256(data).hexdigest(), 'kernel_bytes': len(data),
            'source_input_sha256': current, 'embedded_source_input_sha256': embedded}


def _write_changed(path, data):
    path = Path(path)
    if path.is_file() and path.read_bytes() == data:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)

def prepare_header(repo):
    repo = Path(repo)
    digest = source_digest(repo)
    text = ('/* Derived at build time; excluded from the source input digest. */\n'
            '#ifndef _SYS_REDWOOD_BUILD_PROVENANCE_H_\n'
            '#define _SYS_REDWOOD_BUILD_PROVENANCE_H_\n'
            '#define REDWOOD_BUILD_SOURCE_SHA256 "' + digest + '"\n'
            '#endif\n')
    _write_changed(repo / DERIVED_HEADER, text.encode('utf-8'))
    return digest

def _normalized_bytes(path):
    data = Path(path).read_bytes()
    try:
        return data.decode('utf-8').replace('\r\n', '\n').encode('utf-8')
    except UnicodeDecodeError:
        return data

def prepare_applied_tree(repo, tree, record=False):
    """Bind a fresh apply, or check its receipt before refreshing the derived header."""
    repo, tree = Path(repo), Path(tree)
    digest = prepare_header(repo)
    if not tree.is_dir():
        raise ValueError('applied XNU tree is missing; run xnu26/apply.sh first')
    receipt = tree / '.redwood-source-input-sha256'
    if not record and (not receipt.is_file() or receipt.read_text().strip() != digest):
        raise ValueError('stale applied tree or missing source receipt; use a fresh xnu26/apply.sh tree')
    skip_file = repo / 'xnu26/files.skip'
    skipped = set(skip_file.read_text().splitlines()) if skip_file.is_file() else set()
    expected = {}
    for directory in ('xnu/files', 'xnu26/files'):
        base = repo / directory
        if not base.is_dir():
            continue
        for path in base.rglob('*'):
            if not path.is_file() or '__pycache__' in path.parts:
                continue
            relative = path.relative_to(base).as_posix()
            if directory == 'xnu/files' and relative in skipped:
                continue
            if relative != 'bsd/sys/redwood_build_provenance.h':
                expected[relative] = path
    for relative, source in sorted(expected.items()):
        target = tree / relative
        if not target.is_file() or _normalized_bytes(target) != _normalized_bytes(source):
            raise ValueError('applied overlay differs from current source: ' + relative)
    _write_changed(tree / 'bsd/sys/redwood_build_provenance.h', (repo / DERIVED_HEADER).read_bytes())
    if record:
        _write_changed(receipt, (digest + '\n').encode('ascii'))
    return digest

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel', type=Path, required=True)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        record = kernel_record(args.kernel, args.repo)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    except (OSError, ValueError) as exc:
        print('kernel provenance: ' + str(exc))
        return 1
    return 0
if __name__ == '__main__':
    raise SystemExit(main())
