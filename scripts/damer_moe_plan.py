#!/usr/bin/env python3
"""Compile Damer MoE workload events and export them as a TSV movement plan.

The plan is consumed by apps/damer_moe_bench. Each row is one verified Damer
movement edge:

  name  bytes  fragments  fanout  priority  actions  output_ratio  placement
"""

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_DAMER_ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "Damer"))
DEFAULT_WORKLOAD = os.path.join("examples", "workloads", "moe_alltoall_workload.json")


def as_float(value, fallback=0.0):
    try:
        return float(value)
    except (TypeError, ValueError):
        return fallback


def export_plan(damer_root, workload_path, out_path, max_rdma_ops=128):
    tools = os.path.join(damer_root, "tools")
    if not os.path.isdir(tools):
        raise SystemExit(f"Damer tools not found at {tools} (use --damer-root)")
    sys.path.insert(0, tools)
    import damer_compile
    import benchmark_e2e_speedup as damer_bench

    if not os.path.isabs(workload_path):
        workload_path = os.path.join(damer_root, workload_path)
    with open(workload_path, "r", encoding="utf-8") as f:
        workload = json.load(f)

    rows = []
    modeled_baseline = modeled_damer = 0.0
    for event in workload["events"]:
        plan = damer_compile.compile_plan(
            damer_compile.parse_json_switchlet(event),
            damer_compile.CompileOptions(max_rdma_ops=max_rdma_ops),
        )
        if not plan["verifier"]["ok"]:
            raise SystemExit(f"Damer verifier rejected {plan['switchlet']}: {plan['verifier']}")
        model = damer_bench.modeled_event_latency(event, plan)
        modeled_baseline += model["baseline_us"]
        modeled_damer += model["damer_us"]
        edge = plan["edges"][0]
        rows.append(
            [
                damer_compile.sanitize_c_identifier(str(plan["switchlet"])),
                model["raw_bytes"],
                model["fragments"],
                model["fanout"],
                model["priority"],
                ",".join(a for a in edge["actions"] if a != "move") or "move",
                f"{model['output_ratio']:.6f}",
                edge["placement"],
            ]
        )

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("# name\tbytes\tfragments\tfanout\tpriority\tactions\toutput_ratio\tplacement\n")
        for row in rows:
            f.write("\t".join(str(c) for c in row) + "\n")
    return {
        "workload": workload.get("model"),
        "events": len(rows),
        "modeled_baseline_us": modeled_baseline,
        "modeled_damer_us": modeled_damer,
        "modeled_speedup": modeled_baseline / max(modeled_damer, 0.001),
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--damer-root", default=os.environ.get("DAMER_ROOT", DEFAULT_DAMER_ROOT))
    ap.add_argument("--workload", default=DEFAULT_WORKLOAD)
    ap.add_argument("--out", default="build/apps/damer_moe_bench/moe_plan.tsv")
    ap.add_argument("--max-rdma-ops", type=int, default=128)
    args = ap.parse_args(argv)
    summary = export_plan(args.damer_root, args.workload, args.out, args.max_rdma_ops)
    print(json.dumps({"plan": args.out, **summary}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
