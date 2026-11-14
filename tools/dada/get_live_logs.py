#!/usr/bin/env python3
"""Continuously fetch the xnuloader live log into timestamped local snapshots."""
import argparse
from datetime import datetime
import importlib.util
import math
import subprocess
import sys
import time
from pathlib import Path


def package_root():
    for parent in Path(__file__).resolve().parents:
        if (parent / "Start-K60Pro-iOS26.bat").is_file():
            return parent
    return Path(__file__).resolve().parents[2]


ROOT = package_root()


def validate_package(root):
    path = root / "tools/k60pro/preflight.py"
    spec = importlib.util.spec_from_file_location("k60pro_dada_preflight", path)
    if spec is None or spec.loader is None:
        raise ValueError("Offline package validator is missing: " + str(path))
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    result = module.validate_package(root)
    if not isinstance(result, dict):
        raise ValueError("validate_package(root) must return a dictionary")
    return result


def positive_seconds(value):
    seconds = float(value)
    if not math.isfinite(seconds) or seconds <= 0:
        raise argparse.ArgumentTypeError("timeout must be a finite positive number")
    return seconds


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fastboot', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--interval', type=float, default=5)
    parser.add_argument('--command-timeout', type=positive_seconds, default=20)
    args = parser.parse_args(argv)
    validate_package(ROOT)
    if not args.fastboot.is_file():
        raise ValueError('Fastboot executable is missing: ' + str(args.fastboot))
    if not math.isfinite(args.interval) or args.interval <= 0:
        raise ValueError('--interval must be a finite positive number')

    session = args.output / ('ios-live-' + datetime.now().strftime('%Y%m%d-%H%M%S'))
    session.mkdir(parents=True, exist_ok=True)
    previous = None
    index = 0
    print('Saving live snapshots in ' + str(session), flush=True)
    try:
        while True:
            subprocess.run([str(args.fastboot), '-s', 'xnuloader', 'oem', 'log'], check=True, timeout=args.command_timeout)
            staged = session / 'staged.bin'
            subprocess.run([str(args.fastboot), '-s', 'xnuloader', 'get_staged', str(staged)], check=True, timeout=args.command_timeout)
            data = staged.read_bytes()
            if data != previous:
                index += 1
                previous = data
                stamp = datetime.now().strftime('%Y%m%d-%H%M%S')
                snapshot = session / ('snapshot-%04d-%s.txt' % (index, stamp))
                snapshot.write_bytes(data)
                (args.output / 'latest-ios-log.txt').write_bytes(data)
                print('%s  %s bytes  %s' % (stamp, len(data), snapshot.name), flush=True)
            staged.unlink(missing_ok=True)
            time.sleep(args.interval)
    except KeyboardInterrupt:
        print('\nLog capture stopped.', flush=True)


if __name__ == '__main__':
    main()
