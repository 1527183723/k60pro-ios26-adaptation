"""Prepare a source-bound kernel header before applying or compiling XNU (stdlib)."""
from __future__ import annotations
import argparse
from pathlib import Path
import importlib.util

# The bundled Windows Python uses an isolated _pth; load the sibling explicitly.
_spec = importlib.util.spec_from_file_location(
    'kernel_provenance', Path(__file__).resolve().with_name('kernel_provenance.py'))
_provenance = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_provenance)
prepare_applied_tree = _provenance.prepare_applied_tree
prepare_header = _provenance.prepare_header


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--tree', type=Path)
    parser.add_argument('--record-tree', action='store_true')
    args = parser.parse_args(argv)
    try:
        if args.record_tree and args.tree is None:
            raise ValueError('--record-tree requires a freshly applied --tree')
        if args.tree is None:
            digest = prepare_header(args.repo)
        else:
            digest = prepare_applied_tree(args.repo, args.tree, record=args.record_tree)
    except (OSError, ValueError) as exc:
        print('kernel provenance preparation: ' + str(exc))
        return 1
    print(digest)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
