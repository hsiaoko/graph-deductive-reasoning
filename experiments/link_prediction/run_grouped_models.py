#!/usr/bin/env python3
"""Run resumable NBFNet/A*Net training queues on grouped candidate datasets."""

import argparse
from pathlib import Path
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
DATASETS = ("fb15k-237", "ogbl-biokg", "finreflectkg", "dbpedia")
MODELS = ("nbfnet", "astarnet")


def is_running(model, data):
    pattern = f"path_reasoning.cli train --model {model} --data {data}"
    return subprocess.run(["pgrep", "-f", pattern], stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode == 0


def wait_for_gpu(min_free_gib):
    attempt = 0
    while True:
        output = subprocess.check_output([
            "nvidia-smi", "--query-gpu=memory.free",
            "--format=csv,noheader,nounits"], text=True)
        free = [int(line.strip()) for line in output.splitlines() if line.strip()]
        best = max(range(len(free)), key=lambda i: free[i])
        if free[best] >= min_free_gib * 1024:
            return f"cuda:{best}"
        if attempt % 10 == 0:
            print(f"wait for GPU: best free={free[best] / 1024:.2f} GiB, "
                  f"need={min_free_gib} GiB", flush=True)
        time.sleep(60)
        attempt += 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--datasets", nargs="+", choices=DATASETS, default=DATASETS)
    parser.add_argument("--models", nargs="+", choices=MODELS, default=MODELS)
    parser.add_argument("--device", default="cuda:3")
    parser.add_argument("--epochs", type=int, default=1)
    parser.add_argument("--batch-size", type=int, default=4)
    parser.add_argument("--eval-batch-size", type=int, default=4)
    parser.add_argument("--min-free-gpu-gib", type=int, default=0)
    parser.add_argument("--gradient-checkpointing", action="store_true")
    parser.add_argument("--work", type=Path,
                        default=ROOT / ".work_hdcv/grouped_models")
    args = parser.parse_args()
    if args.device == "auto" and args.min_free_gpu_gib > 0:
        args.device = wait_for_gpu(args.min_free_gpu_gib)
    work = args.work.resolve()
    for name in args.datasets:
        for model in args.models:
            out = work / name / model
            if (out / "summary.json").exists():
                print(f"skip completed {name}/{model}", flush=True)
                continue
            data = work / name / "data"
            if is_running(model, data):
                print(f"skip running {name}/{model}", flush=True)
                continue
            out.mkdir(parents=True, exist_ok=True)
            command = [
                "python3", "-m", "baselines.path_reasoning.cli", "train",
                "--model", model, "--data",
                str(work / name / "data"),
                "--out", str(out), "--device", args.device,
                "--num-layers", "2", "--dim", "32", "--epochs", str(args.epochs),
                "--batch-size", str(args.batch_size),
                "--eval-batch-size", str(args.eval_batch_size),
            ]
            if model == "astarnet":
                command += ["--node-ratio", "0.1", "--degree-ratio", "1.0"]
            if args.gradient_checkpointing:
                command.append("--gradient-checkpointing")
            bounded = [
                "python3", str(ROOT / "experiments/fixpoint_policy/run_bounded.py"),
                "--log", str(out / "run.log"), "--result", str(out / "resource.json"),
                "--timeout-sec", "604800", "--memory-gb", "256",
                "--system-memory-gb", "256", "--", *command,
            ]
            print(f"run {name}/{model}", flush=True)
            subprocess.run(bounded, cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
