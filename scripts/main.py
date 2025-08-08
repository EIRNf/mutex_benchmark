from .runner import run_experiment_iter_single_threaded, run_experiment_lock_level_single_threaded
from .dataloader import load_data_iter, load_data_lock_level, debug_print_averages_lock_level
from .plotter import plot_lock_level, plot_iter, plot_iter_rusage
from .builder import setup, build
from .logger import init_logger
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
        data = load_data_iter()
        if Constants.rusage:
            plot_iter_rusage(data)
        else:
            plot_iter(data)

def main():
    setup()
    init_args()
    build()
    init_logger()

    if Constants.iter:
        run_experiment_iter()
    elif Constants.bench == 'min' or Constants.bench == 'max':
        run_experiment_lock_level()
    else:
        if Constants.bench == 'max':
            run_experiment_lock_level()
        elif Constants.bench == 'grouped':
            run_experiment_iter("threads", iter_range=Constants.iter_threads, thread_level=False)
        elif Constants.bench == 'min':
            run_experiment_lock_level()
        elif Constants.bench == 'kc_cache':
            run_experiment_lock_level()
        else:
            raise NotImplementedError(f"Benchmark {Constants.bench} not recognized.")


if __name__ == "__main__":
    main()