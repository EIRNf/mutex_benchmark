"""Config-driven entry point.

    python -m scripts run  [CONFIG.toml] [-o KEY=VALUE ...] [--dir DIR] [--print-config]
    python -m scripts plot RUN_DIR [-o plot.KEY=VALUE ...]

``run`` executes one experiment into a new run folder (default
data/runs/<timestamp>-<name>/) holding config.toml, run.json, raw/, plots/
and logs/. CONFIG is a TOML experiment file (see experiments/ and the
README); each ``-o`` sets one setting by its dotted key and wins over the
file. With no CONFIG, the run uses the defaults plus the overrides.

``plot`` re-plots a recorded run from its config.toml, without re-running.
Only plot.* settings (and output.log_level) can be overridden there.

The flag CLI (``python -m scripts.main THREADS SECONDS REPS ...``) still works
and shares the same validation.
"""

import argparse
import os
import sys

from .builder import setup
from .config import (
    CONFIG_FILE,
    ConfigError,
    RunPaths,
    apply_to_constants,
    check_host,
    load_config,
    to_toml,
)

_PLOT_OVERRIDABLE = ("plot.", "output.log_level")


def _add_override_option(parser, help_text):
    parser.add_argument(
        "-o", "--override",
        action="append",
        default=[],
        metavar="KEY=VALUE",
        help=help_text,
    )


def _build_parser():
    parser = argparse.ArgumentParser(
        prog="python -m scripts",
        description="Run mutex benchmark experiments from TOML config files.",
    )
    commands = parser.add_subparsers(dest="command", required=True)

    run = commands.add_parser(
        "run",
        help="run and plot one experiment",
        description="Run and plot one experiment configuration into a new run folder.",
    )
    run.add_argument("config", nargs="?", help="TOML experiment file (optional)")
    _add_override_option(run, "set one config value by dotted key; repeatable; wins over the file")
    run.add_argument(
        "--dir",
        help="run folder to create (default: data/runs/<timestamp>-<name>)",
    )
    run.add_argument(
        "--print-config",
        action="store_true",
        help="print the resolved config as TOML and exit without running",
    )

    plot = commands.add_parser(
        "plot",
        help="re-plot a recorded run",
        description="Re-plot a recorded run folder from its config.toml without re-running.",
    )
    plot.add_argument("run_dir", help="run folder containing config.toml")
    _add_override_option(plot, "change a plot.* setting for this re-plot; repeatable")
    return parser


def _run(parser, ns):
    # Resolve user paths before setup() moves into the repository root.
    config_path = os.path.abspath(ns.config) if ns.config else None
    run_dir = os.path.abspath(ns.dir) if ns.dir else None
    setup()
    try:
        cfg = load_config(config_path, ns.override)
        if ns.print_config:
            sys.stdout.write(to_toml(cfg))
            return 0
        check_host(cfg)
        paths = RunPaths.for_new_run(cfg.name, run_dir)
    except ConfigError as exc:
        parser.error(str(exc))

    apply_to_constants(cfg, paths)
    from .main import run_configured  # imports plotting stack; keep --print-config light
    run_configured(cfg)
    print(f"Run folder: {paths.run_dir}")
    return 0


def _plot(parser, ns):
    run_dir = os.path.abspath(ns.run_dir)
    setup()
    try:
        for override in ns.override:
            key = override.partition("=")[0].strip()
            if not key.startswith(_PLOT_OVERRIDABLE):
                raise ConfigError(
                    f"only plot.* settings can change when re-plotting (got `{key}`); "
                    "use `run` for a new experiment"
                )
        paths = RunPaths.for_existing_run(run_dir)
        cfg = load_config(os.path.join(run_dir, CONFIG_FILE), ns.override)
        if cfg.plot.layout == "none":
            raise ConfigError("this run has `plot.layout = none`; add -o plot.layout=auto")
    except ConfigError as exc:
        parser.error(str(exc))

    from .rundir import read_record
    record = read_record(run_dir)
    if record and record.get("status") != "complete":
        print(f"note: this run's status is {record.get('status')!r}; plotting the data it wrote",
              file=sys.stderr)

    apply_to_constants(cfg, paths)
    from .main import run_configured
    run_configured(cfg)
    print(f"Plots: {paths.plots}")
    return 0


def main(argv=None):
    parser = _build_parser()
    ns = parser.parse_args(argv)
    if ns.command == "run":
        return _run(parser, ns)
    if ns.command == "plot":
        return _plot(parser, ns)
    parser.error(f"unknown command {ns.command!r}")  # pragma: no cover


if __name__ == "__main__":
    sys.exit(main())
