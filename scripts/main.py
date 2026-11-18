from .runner import run_experiment_iter_single_threaded, run_experiment_lock_level_single_threaded
from .dataloader import load_data_iter, load_data_lock_level, debug_print_averages_lock_level
from .plotter import plot_lock_level, plot_iter, plot_iter_rusage, plot_variability, plot_throughput_heatmap, print_speedup_table
from .builder import setup, build, prepare_output_dirs, verify_build_matches
from .logger import init_logger, logger
from .args import init_args
from .constants import Constants
from .rundir import RunRecord


def run_experiment_lock_level(run):
    if not run.skip_experiment:
        run_experiment_lock_level_single_threaded(run)
    if run.averages:
        debug_print_averages_lock_level(run)
    if not run.skip_plotting:
        data = load_data_lock_level(run)
        plot_lock_level(run, data)


def run_experiment_iter(run):
    if not run.skip_experiment:
        run_experiment_iter_single_threaded(run)
    if not run.skip_plotting:
        data = list(load_data_iter(run))
        if run.rusage:
            plot_iter_rusage(run, data)
        else:
            plot_iter(run, data)
            if not run.no_heatmap:
                ref = run.heatmap_ref or run.speedup_ref
                if ref is None:
                    ref = Constants.Defaults.HEATMAP_REFERENCE
                    if ref not in run.mutex_names and data:
                        ref = data[0][0]
                        logger.warning("Default heatmap reference is not selected; using '%s'", ref)
                plot_throughput_heatmap(run, data, ref)
        if run.variability:
            plot_variability(run, data)
    if run.speedup:
        data = load_data_iter(run)
        print_speedup_table(run, data)


def _dispatch(run):
    if run.capture == "latency" and run.iter:
        # Capture-only mode: run sweep and write CSVs without plotting.
        if not run.skip_experiment:
            run_experiment_iter_single_threaded(run)
    elif run.capture == "latency":
        run_experiment_lock_level(run)
    elif run.capture in ("throughput", "rusage"):
        run_experiment_iter(run)
    else:
        raise NotImplementedError(f"Capture mode '{run.capture}' not recognized")


def _build_or_reuse(run, record):
    """Build run.build_dir (unless skipped) and check it fits this run."""
    if run.skip_build:
        logger.info("Reusing the existing build in %s/", run.build_dir)
    else:
        build(
            run.build_dir,
            hardware_cxl=run.hardware_cxl,
            mutex_names=run.mutex_names,
            tigon=run.bench.startswith("tigon"),
        )
    cpp_args = verify_build_matches(
        run.build_dir,
        hardware_cxl=run.hardware_cxl,
        mutex_names=run.mutex_names,
        placement=run.placement,
    )
    if record:
        record.update(build={
            "dir": run.build_dir,
            "cpp_args": cpp_args,
            "skipped": run.skip_build,
        })


def run_configured(run):
    """Run (or, when reusing data, only plot) one experiment from its RunSettings.

    A new run writes config.toml and run.json into its run folder before
    building, and run.json's final status says whether it completed, failed
    or was interrupted, plus which data points are missing.
    """
    prepare_output_dirs(run)
    init_logger(run)
    if run.experiment_name:
        logger.info("Experiment: %s", run.experiment_name)

    record = None
    if not run.skip_experiment and run.run_dir:
        record = RunRecord(run)
        record.start()
        logger.info("Run folder: %s", run.run_dir)

    try:
        if not run.skip_experiment:
            _build_or_reuse(run, record)
        _dispatch(run)
    except KeyboardInterrupt:
        if record:
            record.finish("interrupted")
        raise
    except BaseException as exc:
        if record:
            record.finish("failed", error=f"{type(exc).__name__}: {exc}")
        raise
    if record:
        record.finish("complete")
        points = record.data["data_points"]
        if points["written"] < points["expected"]:
            logger.warning(
                "%d of %d data points are missing (hung or failed); see %s",
                points["expected"] - points["written"], points["expected"], record.record_path,
            )


def main():
    setup()
    run_configured(init_args())


if __name__ == "__main__":
    main()
