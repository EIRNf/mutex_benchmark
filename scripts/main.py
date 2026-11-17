from .runner import run_experiment_iter_single_threaded, run_experiment_lock_level_single_threaded
from .dataloader import load_data_iter, load_data_lock_level, debug_print_averages_lock_level
from .plotter import plot_lock_level, plot_iter, plot_iter_rusage, plot_variability, plot_throughput_heatmap, print_speedup_table
from .builder import setup, build
from .logger import init_logger, logger
from .args import init_args
from .constants import Constants

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

def run_configured():
    """Build (unless reusing data), then run and plot the experiment in Constants."""
    if not Constants.skip_experiment:
        build()
    init_logger()
    if Constants.experiment_name:
        logger.info("Experiment: %s", Constants.experiment_name)

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


def main():
    setup()
    init_args()
    run_configured()


if __name__ == "__main__":
    main()