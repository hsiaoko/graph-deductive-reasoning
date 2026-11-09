#!/usr/bin/env python3
import argparse
import csv
from pathlib import Path

import run_fixpoint_q as base

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / ".work_hdcv/fixpoint_q640_kernels"
MODEL = ROOT / ".work_hdcv/fixpoint_q640/models/q_hdc.meta"
BACKENDS = ("original", "simd", "bit-lut")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--phase", choices=("eval",), default="eval")
    args = parser.parse_args()
    del args
    base.gate()
    with base.TEST_GOALS.open() as stream:
        goals = list(csv.DictReader(stream, delimiter="\t"))
    for goal in goals:
        for backend in BACKENDS:
            name = f"{goal['goal_id']}/{backend}"
            output = WORK / "runs" / name
            command = base.agent_command(goal, output) + [
                "--policy", "q-hdc", "--value-model", str(MODEL),
                "--hdc-value-backend", backend, "--fair-interval", "1024",
            ]
            base.run(name, command, WORK / "runs")


if __name__ == "__main__":
    main()
