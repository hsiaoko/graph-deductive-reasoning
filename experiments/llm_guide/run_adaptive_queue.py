#!/usr/bin/env python3
import argparse
import csv
import json
import os
from pathlib import Path
import subprocess
import time

import run_full_queue as infra

ROOT = Path(__file__).resolve().parents[2]
TRAIN_GOALS = ROOT / ".work_hdcv/fixpoint_q640/train_goals.tsv"
MODEL = ROOT / ".work_hdcv/relevance640/models/selected.meta"
PROBE = ROOT / ".work_hdcv/adaptive_gate_train_probe_cpu"
GUIDED = ROOT / ".work_hdcv/adaptive_gate_train_guided_cpu"
SELECTION = ROOT / ".work_hdcv/adaptive_gate_selection_cpu.json"
TEST = ROOT / ".work_hdcv/adaptive_fb15k_cpu"
STATUS = ROOT / ".work_hdcv/adaptive_queue_status.json"


def write_status(state, **extra):
    payload = {"state": state,
               "updated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
               **extra}
    STATUS.write_text(json.dumps(payload, indent=2) + "\n")


def guided_complete():
    path = GUIDED / "rows.tsv"
    if not path.exists():
        return False
    with path.open() as stream:
        rows = list(csv.DictReader(stream, delimiter="\t"))
    return (len(rows) == 36 and all(row["arm"] == "jev" for row in rows) and
            all(row["status"] == "completed" for row in rows) and
            all(row["sound"] == "1" for row in rows) and
            all(int(row["guide_fallbacks"]) == 0 for row in rows))


def run_guided():
    for attempt in range(1, 5):
        infra.ensure_service()
        recovered = infra.recover_fallbacks(GUIDED)
        write_status("guided", attempt=attempt, recovered_fallbacks=recovered)
        subprocess.run([
            "python3", str(ROOT / "experiments/llm_guide/run_fb15k_jev.py"),
            "--work", str(GUIDED), "--goals", str(TRAIN_GOALS),
            "--base-policy", "adaptive", "--relevance-model", str(MODEL), "--hdc-dim", "128",
            "--arm", "jev", "--guide-min-cost", "0",
            "--guide-max-options", "64", "--phase", "all",
        ], cwd=ROOT, check=False)
        if guided_complete():
            return
        time.sleep(15)
    raise RuntimeError("guided training did not complete cleanly")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--guided-pid", type=int)
    parser.add_argument("--existing-jev-pid", type=int)
    args = parser.parse_args()
    infra.STATUS = STATUS
    infra.SERVICE_LOG = ROOT / ".work_hdcv/adaptive_jev_service.log"
    infra.SERVICE_PID = ROOT / ".work_hdcv/adaptive_jev_service.pid"
    if args.existing_jev_pid is not None:
        infra.SERVICE_PIDS.add(args.existing_jev_pid)
    try:
        write_status("queue_started", pid=os.getpid())
        infra.ensure_service()
        while not guided_complete() and infra.pid_alive(args.guided_pid):
            write_status("waiting_for_existing_guided", pid=args.guided_pid)
            time.sleep(15)
            infra.ensure_service()
        if not guided_complete():
            run_guided()
        write_status("selecting_gate")
        result = subprocess.run([
            "python3", str(ROOT / "experiments/llm_guide/select_adaptive_gate.py"),
            "--probe", str(PROBE), "--guided", str(GUIDED),
            "--out", str(SELECTION),
        ], cwd=ROOT, check=False)
        if result.returncode != 0 or not SELECTION.exists():
            write_status("complete", outcome="no_valid_training_gate")
            return
        selected = json.loads(SELECTION.read_text())
        if selected["train_speedup"] <= 1.0:
            write_status("complete", outcome="no_training_gain",
                         selection=str(SELECTION))
            return
        write_status("testing", selection=str(SELECTION))
        subprocess.run([
            "python3", str(ROOT / "experiments/llm_guide/run_fb15k_jev.py"),
            "--work", str(TEST), "--base-policy", "adaptive",
            "--relevance-model", str(MODEL), "--hdc-dim", "128", "--guide-min-cost",
            str(selected["selected_threshold"]), "--guide-max-options", "64",
            "--phase", "all",
        ], cwd=ROOT, check=True)
        write_status("complete", outcome="tested", selection=str(SELECTION),
                     report=str(TEST / "summary.md"))
    except Exception as error:
        write_status("failed", error=str(error))
        raise
    finally:
        infra.shutdown_services()
        if STATUS.exists():
            payload = json.loads(STATUS.read_text())
            payload["jev_service"] = "stopped"
            STATUS.write_text(json.dumps(payload, indent=2) + "\n")


if __name__ == "__main__":
    main()
