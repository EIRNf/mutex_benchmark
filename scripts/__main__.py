"""Config-driven entry point.

    python -m scripts run   [CONFIG.toml] [-o KEY=VALUE ...] [--dir DIR] [--print-config]
    python -m scripts plot  RUN_DIR [-o plot.KEY=VALUE ...]
    python -m scripts suite SUITE.toml [-o KEY=VALUE ...] [--list] [--only GLOB ...]
    python -m scripts suite --resume SUITE_DIR

``run`` executes one experiment into a new run folder (default
data/runs/<timestamp>-<name>/) holding config.toml, run.json, raw/, plots/
and logs/. CONFIG is a TOML experiment file (see experiments/ and the
README); each ``-o`` sets one setting by its dotted key and wins over the
file. With no CONFIG, the run uses the defaults plus the overrides.

``plot`` re-plots a recorded run from its config.toml, without re-running.
Only plot.* settings (and output.log_level) can be overridden there.

``suite`` runs a set of distinct experiments (see scripts/suite.py), each in
its own run folder under data/suites/<timestamp>-<name>/, and can resume a
suite that stopped part way.

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
    run.add_argument(
        "--build-dir",
        help="Meson build folder (default: build/, or build-hardware_cxl/ for placement = locks)",
    )
    run.add_argument(
        "--no-build",
        action="store_true",
        help="reuse the build folder as is; it is still checked against the config",
    )

    plot = commands.add_parser(
        "plot",
        help="re-plot a recorded run",
        description="Re-plot a recorded run folder from its config.toml without re-running.",
    )
    plot.add_argument("run_dir", help="run folder containing config.toml")
    _add_override_option(plot, "change a plot.* setting for this re-plot; repeatable")

    suite = commands.add_parser(
        "suite",
        help="run a set of experiments",
        description="Run a suite of distinct experiments, each into its own run folder, "
                    "or resume one that stopped part way.",
    )
    suite.add_argument("suite_file", nargs="?", help="TOML suite file (see experiments/suites/)")
    suite.add_argument(
        "--resume",
        metavar="SUITE_DIR",
        help="continue a suite folder: run every run that is not complete",
    )
    _add_override_option(suite, "set one setting in every experiment by dotted key; repeatable")
    suite.add_argument("--dir", help="suite folder to create (default: data/suites/<timestamp>-<name>)")
    suite.add_argument(
        "--list",
        action="store_true",
        help="print the runs, builds and estimated time, then exit without running",
    )
    suite.add_argument(
        "--only",
        action="append",
        default=[],
        metavar="GLOB",
        help="run only runs whose key, experiment or environment matches, "
             "e.g. 'cxl/*', 'max' or 'dram'; repeatable",
    )
    suite.add_argument(
        "--no-build",
        action="store_true",
        help="reuse existing build folders (each run still checks that its build fits)",
    )
    suite.add_argument(
        "--stop-on-failure",
        action="store_true",
        help="stop at the first failed run (default: carry on with the rest)",
    )
    return parser


def _run(parser, ns):
    # Resolve user paths before setup() moves into the repository root.
    config_path = os.path.abspath(ns.config) if ns.config else None
    run_dir = os.path.abspath(ns.dir) if ns.dir else None
    build_dir = os.path.abspath(ns.build_dir) if ns.build_dir else None
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

    apply_to_constants(cfg, paths, build_dir=build_dir, skip_build=True if ns.no_build else None)
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


def _suite(parser, ns):
    if bool(ns.suite_file) == bool(ns.resume):
        parser.error("give a suite file, or --resume SUITE_DIR (not both)")
    if ns.resume and (ns.override or ns.dir):
        parser.error("--resume runs the recorded plan; -o and --dir cannot change it")
    suite_file = os.path.abspath(ns.suite_file) if ns.suite_file else None
    suite_dir = os.path.abspath(ns.resume or ns.dir) if (ns.resume or ns.dir) else None
    setup()

    from .suite import (
        SuiteOptions, SuiteRunner, describe_suite, load_suite, new_suite_dir, suite_from_record,
    )
    try:
        if ns.resume:
            suite = suite_from_record(suite_dir)
        else:
            suite = load_suite(suite_file, ns.override)
            suite_dir = suite_dir or os.path.abspath(new_suite_dir(suite.name))
        options = SuiteOptions(only=ns.only, no_build=ns.no_build, stop_on_failure=ns.stop_on_failure)
        runner = SuiteRunner(suite, suite_dir, options)
        selected = runner.selected_runs()
        if ns.list:
            statuses = {run.key: runner.status(run) for run in suite.runs} if ns.resume else None
            print(describe_suite(suite, statuses))
            if ns.only:
                print(f"--only selects {len(selected)} of {len(suite.runs)} runs: "
                      + ", ".join(run.key for run in selected))
            return 0
        if not ns.resume:
            runner.create(source=suite_file)
            print(f"Suite folder: {suite_dir}")
        code = runner.run()
    except ConfigError as exc:
        parser.error(str(exc))
    except KeyboardInterrupt:
        print(f"\nInterrupted. Resume with: python -m scripts suite --resume {suite_dir}", file=sys.stderr)
        return 130
    if code != 0:
        print(f"Some runs did not complete. Resume with: python -m scripts suite --resume {suite_dir}",
              file=sys.stderr)
    return code


def main(argv=None):
    parser = _build_parser()
    ns = parser.parse_args(argv)
    if ns.command == "run":
        return _run(parser, ns)
    if ns.command == "plot":
        return _plot(parser, ns)
    if ns.command == "suite":
        return _suite(parser, ns)
    parser.error(f"unknown command {ns.command!r}")  # pragma: no cover


if __name__ == "__main__":
    sys.exit(main())
