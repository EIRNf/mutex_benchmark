"""CPU topology from Linux sysfs, and the CPU orders used to pin benchmark threads.

Thread i of a pinned run is pinned to the i-th CPU of an order (wrapping around
when there are more threads than CPUs, which is oversubscription):

  compact  one thread per physical core, filling socket 0's cores, then
           socket 1's, and so on; SMT siblings (hyperthreads) only after every
           physical core has a thread. Thread counts up to one socket's core
           count stay inside one socket.
  scatter  one thread per physical core, alternating sockets (core 0 of each
           socket, then core 1 of each, ...); SMT siblings last.

Only CPUs this process may run on (its affinity mask, e.g. from taskset or a
cpuset cgroup) are used.
"""

from __future__ import annotations

import functools
import os
from dataclasses import dataclass

SYSFS = "/sys/devices/system"
PIN_ORDERS = ("compact", "scatter")


class TopologyError(RuntimeError):
    """The CPU topology could not be read; the message is user-facing."""


@dataclass(frozen=True)
class Cpu:
    cpu: int
    package: int        # socket
    core: int           # lowest CPU number among its SMT siblings: one value per physical core
    smt: int            # position among its SMT siblings (0 = the first hyperthread)
    node: int | None    # NUMA node, when sysfs lists it

    def as_list(self) -> list:
        return [self.cpu, self.package, self.core, self.smt, self.node]


def parse_cpu_list(text: str) -> list[int]:
    """Parse a sysfs CPU list such as "0-3,8,10-11"."""
    cpus = []
    for part in text.strip().split(","):
        part = part.strip()
        if not part:
            continue
        first, _, last = part.partition("-")
        try:
            low, high = int(first), int(last) if last else int(first)
        except ValueError:
            raise TopologyError(f"unexpected CPU list {text!r}") from None
        cpus.extend(range(low, high + 1))
    return cpus


def _read(path: str) -> str:
    try:
        with open(path, encoding="utf-8") as handle:
            return handle.read()
    except OSError as exc:
        raise TopologyError(f"cannot read {path}: {exc.strerror}") from None


def _numa_nodes(root: str) -> dict[int, int]:
    """cpu -> NUMA node, from <root>/node/node*/cpulist (empty when absent)."""
    nodes = {}
    node_dir = os.path.join(root, "node")
    try:
        names = os.listdir(node_dir)
    except OSError:
        return nodes
    for name in names:
        if not (name.startswith("node") and name[4:].isdigit()):
            continue
        try:
            cpus = parse_cpu_list(_read(os.path.join(node_dir, name, "cpulist")))
        except TopologyError:
            continue
        for cpu in cpus:
            nodes[cpu] = int(name[4:])
    return nodes


def read_topology(root: str = SYSFS, allowed: set[int] | None = None) -> list[Cpu]:
    """Every online CPU (limited to `allowed`, if given), from sysfs under root."""
    online = parse_cpu_list(_read(os.path.join(root, "cpu", "online")))
    if allowed is not None:
        online = [cpu for cpu in online if cpu in allowed]
    if not online:
        raise TopologyError("no online CPUs that this process may run on")
    nodes = _numa_nodes(root)
    cpus = []
    for cpu in online:
        topology = os.path.join(root, "cpu", f"cpu{cpu}", "topology")
        try:
            package = int(_read(os.path.join(topology, "physical_package_id")).strip())
        except ValueError:
            raise TopologyError(f"unexpected physical_package_id for CPU {cpu}") from None
        siblings_path = os.path.join(topology, "core_cpus_list")
        if not os.path.exists(siblings_path):
            siblings_path = os.path.join(topology, "thread_siblings_list")
        siblings = sorted(parse_cpu_list(_read(siblings_path)))
        if cpu not in siblings:
            siblings = sorted([*siblings, cpu])
        cpus.append(Cpu(cpu, package, siblings[0], siblings.index(cpu), nodes.get(cpu)))
    return cpus


def cpu_order(mode: str, cpus: list[Cpu]) -> list[int]:
    """The CPUs in the order threads are pinned to them (see the module docstring)."""
    if mode == "compact":
        key = lambda c: (c.smt, c.package, c.core, c.cpu)  # noqa: E731
    elif mode == "scatter":
        packages: dict[int, list[int]] = {}
        for c in cpus:
            cores = packages.setdefault(c.package, [])
            if c.core not in cores:
                cores.append(c.core)
        rank = {
            (package, core): index
            for package, cores in packages.items()
            for index, core in enumerate(sorted(cores))
        }
        key = lambda c: (c.smt, rank[(c.package, c.core)], c.package, c.cpu)  # noqa: E731
    else:
        raise ValueError(f"unknown pin order {mode!r}; choose from {', '.join(PIN_ORDERS)}")
    return [c.cpu for c in sorted(cpus, key=key)]


def pinned_cpus(order: list[int], threads: int) -> list[int]:
    """The CPU for each of `threads` threads: the order, wrapping around."""
    return [order[i % len(order)] for i in range(threads)]


def _allowed_cpus() -> set[int] | None:
    try:
        return set(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        return None


@functools.lru_cache(maxsize=None)
def host_cpus() -> tuple[Cpu, ...]:
    """This host's CPUs that this process may use (Linux only)."""
    return tuple(read_topology(SYSFS, _allowed_cpus()))


def host_cpu_order(mode: str) -> list[int]:
    return cpu_order(mode, list(host_cpus()))
