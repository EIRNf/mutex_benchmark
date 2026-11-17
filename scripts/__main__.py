"""Config-driven entry point.

    python -m scripts run [CONFIG.toml] [-o KEY=VALUE ...] [--print-config]

CONFIG is a TOML experiment file (see experiments/ and the README). Each
``-o`` sets one setting by its dotted key and wins over the file, e.g.
``-o seconds=10 -o memory.node=1 -o locks.include=spin,mcs``. With no CONFIG,
the run uses the defaults plus the overrides.

The flag CLI (``python -m scripts.main THREADS SECONDS REPS ...``) still works
and shares the same validation.
"""

import argparse
import os
import sys

from .builder import setup
from .config import ConfigError, apply_to_constants, check_host, load_config, to_toml


def _build_parser():
    parser = argparse.ArgumentParser(
        prog="python -m scripts",
        description="Run mutex benchmark experiments from TOML config files.",
    )
    commands = parser.add_subparsers(dest="command", required=True)

    run = commands.add_parser(
        "run",
        help="run and plot one experiment",
        description="Run and plot one experiment configuration.",
    )
    run.add_argument("config", nargs="?", help="TOML experiment file (optional)")
    run.add_argument(
        "-o", "--override",
        action="append",
        default=[],
        metavar="KEY=VALUE",
        help="set one config value by dotted key; repeatable; wins over the file",
    )
    run.add_argument(
        "--print-config",
        action="store_true",
        help="print the resolved config as TOML and exit without running",
    )
    return parser


def _run(parser, ns):
    config_path = os.path.abspath(ns.config) if ns.config else None  # before setup() chdirs
    setup()
    try:
        cfg = load_config(config_path, ns.override)
        if ns.print_config:
            sys.stdout.write(to_toml(cfg))
            return 0
        check_host(cfg)
    except ConfigError as exc:
        parser.error(str(exc))

    apply_to_constants(cfg)
    from .main import run_configured  # imports plotting stack; keep --print-config light
    run_configured()
    return 0


def main(argv=None):
    parser = _build_parser()
    ns = parser.parse_args(argv)
    if ns.command == "run":
        return _run(parser, ns)
    parser.error(f"unknown command {ns.command!r}")  # pragma: no cover


if __name__ == "__main__":
    sys.exit(main())
