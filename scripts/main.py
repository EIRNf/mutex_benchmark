import os

from .runner import run_experiment_iter_single_threaded, run_experiment_lock_level_single_threaded
from .dataloader import load_data_iter, load_data_lock_level, debug_print_averages_lock_level
from .plotter import plot_lock_level, plot_iter, plot_iter_rusage, plot_variability, plot_throughput_heatmap, print_speedup_table
from .builder import setup, build, prepare_output_dirs, verify_build_matches
from .logger import init_logger, logger
from .args import init_args
from .constants import Constants
from .rundir import RunRecord

def run_experiment_lock_level():
    if not Constants.skip_experiment:
        run_experiment_lock_level_single_threaded()
    if Constants.averages:
        debug_print_averages_lock_level()
    if not Constants.skip_plotting:
        data = load_data_lock_level()
        plot_lock_level(data)

def run_experiment_iter():
    if not Constants.skip_experiment:
        run_experiment_iter_single_threaded()
    if not Constants.skip_plotting:
        data = list(load_data_iter())
        if Constants.rusage:
            plot_iter_rusage(data)
        else:
            plot_iter(data)
            if not Constants.no_heatmap:
                ref = Constants.heatmap_ref or Constants.speedup_ref
                if ref is None:
                    ref = Constants.Defaults.HEATMAP_REFERENCE
                    if ref not in Constants.mutex_names and data:
                        ref = data[0][0]
                        logger.warning("Default heatmap reference is not selected; using '%s'", ref)
                plot_throughput_heatmap(data, ref)
        if getattr(Constants, 'variability', False):
            plot_variability(data)
    if getattr(Constants, 'speedup', False):
        data = load_data_iter()
        print_speedup_table(data)

def _dispatch():
    if Constants.capture == "latency" and Constants.iter:
        # Capture-only mode: run sweep and write CSVs without plotting.
        if not Constants.skip_experiment:
            run_experiment_iter_single_threaded()
    elif Constants.capture == "latency":
        run_experiment_lock_level()
    elif Constants.capture in ("throughput", "rusage"):
        run_experiment_iter()
    else:
        raise NotImplementedError(f"Capture mode '{Constants.capture}' not recognized")


def run_configured(cfg):
    """Run (or, when reusing data, only plot) the experiment loaded into Constants.

    A new run writes config.toml and run.json into its run folder before
    building, and run.json's final status says whether it completed, failed
    or was interrupted, plus which data points are missing.
    """
    prepare_output_dirs()
    init_logger()
    if Constants.experiment_name:
        logger.info("Experiment: %s", Constants.experiment_name)

    record = None
    if not Constants.skip_experiment and Constants.run_dir:
        record = RunRecord(cfg, Constants.run_dir)
        record.start()
        logger.info("Run folder: %s", Constants.run_dir)

    try:
        if not Constants.skip_experiment:
            build()
            cpp_args = verify_build_matches()
            if record:
                record.update(build={
                    "cpp_args": cpp_args,
                    "skipped": os.environ.get("MUTEX_BENCH_SKIP_BUILD") == "1",
                })
        _dispatch()
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
    cfg = init_args()
    run_configured(cfg)


if __name__ == "__main__":
    main()