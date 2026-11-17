#!/usr/bin/env python3
import json
import os
from pathlib import Path
import subprocess
import time

import run_full_queue as infra

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / ".work_hdcv/adaptive_fb15k_cpu_fixed"
STATUS = ROOT / ".work_hdcv/adaptive_fb15k_cpu_fixed_status.json"
MODEL = ROOT / ".work_hdcv/fixpoint_q640/models/q_numeric.meta"


def write_status(state, **extra):
    payload = {"state": state,
               "updated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
               **extra}
    STATUS.write_text(json.dumps(payload, indent=2) + "\n")


def main():
    infra.STATUS = STATUS
    infra.SERVICE_LOG = ROOT / ".work_hdcv/adaptive_fb15k_cpu_fixed_jev.log"
    infra.SERVICE_PID = ROOT / ".work_hdcv/adaptive_fb15k_cpu_fixed_jev.pid"
    try:
        write_status("queue_started", pid=os.getpid())
        infra.ensure_service()
        for attempt in range(1, 5):
            recovered = infra.recover_fallbacks(WORK)
            write_status("running", attempt=attempt,
                         recovered_fallbacks=recovered)
            result = subprocess.run([
                "python3", str(ROOT / "experiments/llm_guide/run_fb15k_jev.py"),
                "--work", str(WORK), "--engine", "cpu",
                "--base-policy", "adaptive", "--value-model", str(MODEL),
                "--guide-min-cost", "0", "--guide-max-options", "64",
                "--phase", "all",
            ], cwd=ROOT, check=False)
            if result.returncode == 0 and infra.summary_complete(WORK):
                write_status("complete", runs=216,
                             summary=str(WORK / "summary.md"))
                return
            write_status("retrying", attempt=attempt,
                         returncode=result.returncode)
            infra.ensure_service()
        raise RuntimeError("CPU adaptive matrix did not complete cleanly")
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
