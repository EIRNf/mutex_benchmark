import sys

if sys.version_info < (3, 11):
    raise SystemExit(
        "mutex_benchmark's Python scripts require Python 3.11 or newer "
        f"(found {sys.version.split()[0]}); see requirements.txt"
    )
