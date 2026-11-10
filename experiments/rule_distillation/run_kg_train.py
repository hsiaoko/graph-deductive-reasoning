#!/usr/bin/env python3
"""Run full rule deduction and generate train/ data for the available KGs."""

import argparse
import json
from pathlib import Path
import subprocess
import time

import yaml

ROOT = Path(__file__).resolve().parents[2]
KG = Path("/ssd_data/zhuxk/graphs/KG")
DATASETS = ("fb15k-237", "ogbl-biokg", "finreflectkg", "dbpedia")


def memory_ok():
    used = int(subprocess.check_output(["free", "-b"], text=True).splitlines()[1].split()[2])
    return used <= 210 * (1 << 30)


def gate():
    deadline = time.monotonic() + 900
    while not memory_ok():
        if time.monotonic() >= deadline:
            raise RuntimeError("memory gate did not open within 900s")
        time.sleep(30)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--datasets", nargs="+", choices=DATASETS, default=DATASETS)
    parser.add_argument("--phase", choices=("deduce", "generate", "all"), default="all")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--negative-ratio", type=int, default=1)
    parser.add_argument("--max-positive", type=int, default=1_000_000)
    args = parser.parse_args()

    def usable(summary):
        if summary.get("fixpoint") is True:
            return True
        return int(summary.get("derived_total", 0)) >= args.max_positive

    for name in args.datasets:
        gate()
        dataset = KG / name
        work = ROOT / ".work_hdcv/kg_train" / name
        closure = work / "closure"
        closure.mkdir(parents=True, exist_ok=True)
        summary_path = closure / "summary.yaml"
        ready = False
        if summary_path.exists():
            summary = yaml.safe_load(summary_path.read_text())
            ready = usable(summary)
        if args.phase in ("deduce", "all") and not ready:
            command = [
                "./build/bin/gar_reason", "--mode", "full", "--engine", "cpu",
                "--csr", str(dataset / "immutable_csr"),
                "--rules", str(dataset / "rules"), "--out", str(closure),
                "--threads", "0", "--max-rounds", "1000",
                "--max-derivations", str(args.max_positive),
                "--max-matches", "1000000000",
                "--no-matches", "--print", "0",
            ]
            bounded = [
                "python3", str(ROOT / "experiments/fixpoint_policy/run_bounded.py"),
                "--log", str(closure / "run.log"), "--result", str(closure / "resource.json"),
                "--timeout-sec", "1000", "--memory-gb", "256",
                "--system-memory-gb", "256", "--", *command,
            ]
            print(f"deduce {name}", flush=True)
            subprocess.run(bounded, cwd=ROOT, check=False)
            if summary_path.exists():
                summary = yaml.safe_load(summary_path.read_text())
                ready = usable(summary)
        if args.phase in ("generate", "all"):
            if not ready:
                raise RuntimeError(
                    f"{name}: deduction reached neither fixpoint nor the positive cap")
            out = dataset / "train"
            if (out / "manifest.json").exists():
                print(f"skip existing {out}", flush=True)
                continue
            command = [
                "python3", str(ROOT / "experiments/rule_distillation/generate_kg_train.py"),
                "--graph", str(dataset / "triple_graph"), "--closure",
                str(closure / "closure_edges.tsv"), "--rules", str(dataset / "rules"),
                "--out", str(out), "--seed", str(args.seed),
                "--negative-ratio", str(args.negative_ratio),
            ]
            if args.max_positive is not None:
                command += ["--max-positive", str(args.max_positive)]
            print(f"generate {name}", flush=True)
            subprocess.run(command, cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
