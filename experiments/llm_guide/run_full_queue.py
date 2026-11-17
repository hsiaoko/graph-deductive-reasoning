#!/usr/bin/env python3
import argparse
import csv
import json
import os
from pathlib import Path
import signal
import subprocess
import time
from urllib.error import HTTPError, URLError
from urllib.request import urlopen

ROOT = Path(__file__).resolve().parents[2]
RUNNER = ROOT / "experiments/llm_guide/run_fb15k_jev.py"
REPORT = ROOT / "experiments/llm_guide/write_full_report.py"
STATUS = ROOT / ".work_hdcv/fb15k_full_queue_status.json"
SERVICE_LOG = ROOT / ".work_hdcv/fb15k_jev_service.log"
SERVICE_PID = ROOT / ".work_hdcv/fb15k_jev_service.pid"
SERVICE_PIDS = set()


def write_status(state, **extra):
    payload = {"state": state, "updated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), **extra}
    STATUS.parent.mkdir(parents=True, exist_ok=True)
    STATUS.write_text(json.dumps(payload, indent=2) + "\n")


def pid_alive(pid):
    if pid is None:
        return False
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def service_state():
    try:
        with urlopen("http://127.0.0.1:30000/health", timeout=5) as response:
            return "ready" if response.status == 200 else "starting"
    except HTTPError:
        return "starting"
    except (URLError, TimeoutError, ConnectionResetError):
        return "down"


def ensure_service():
    if service_state() == "ready":
        return
    if service_state() == "down":
        env = dict(os.environ)
        env.update({
            "CUDA_VISIBLE_DEVICES": "1",
            "JEV_VENV_BIN": str(ROOT / "third_party/llm2jev/.venv-runtime/bin"),
            "JEV_MODEL_PATH": "/ssd_data/zhuxk/models/Qwen3-1.7B",
        })
        SERVICE_LOG.parent.mkdir(parents=True, exist_ok=True)
        with SERVICE_LOG.open("a") as log:
            process = subprocess.Popen(
                [str(ROOT / "deploy/jev/serve_local.sh")],
                cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
                start_new_session=True)
        SERVICE_PID.write_text(str(process.pid) + "\n")
        SERVICE_PIDS.add(process.pid)
        write_status("starting_jev", pid=process.pid)
    deadline = time.monotonic() + 600
    while time.monotonic() < deadline:
        if service_state() == "ready":
            return
        time.sleep(10)
    raise RuntimeError("Jev service did not become ready within 600 seconds")


def signal_service(pid, sig):
    try:
        if os.getpgid(pid) == pid:
            os.killpg(pid, sig)
        else:
            os.kill(pid, sig)
    except ProcessLookupError:
        pass


def shutdown_services():
    for pid in SERVICE_PIDS:
        if pid_alive(pid):
            signal_service(pid, signal.SIGTERM)
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline and service_state() != "down":
        time.sleep(2)
    if service_state() != "down":
        for pid in SERVICE_PIDS:
            if pid_alive(pid):
                signal_service(pid, signal.SIGKILL)
    SERVICE_PIDS.clear()


def fallback_runs(work):
    runs = work / "runs"
    if not runs.exists():
        return []
    failed = []
    for summary in runs.glob("*/*/*/summary.yaml"):
        if summary.parent.parent.name != "jev":
            continue
        for line in summary.read_text().splitlines():
            if line.startswith("  fallbacks:") and int(line.split(":", 1)[1]) > 0:
                failed.append(summary.parent)
                break
    return failed


def recover_fallbacks(work):
    recovered = []
    for index, run in enumerate(fallback_runs(work)):
        destination_root = work / "recovered"
        destination_root.mkdir(parents=True, exist_ok=True)
        destination = destination_root / (
            f"{run.parent.parent.name}_{run.parent.name}_{run.name}_"
            f"{int(time.time())}_{index}")
        run.rename(destination)
        recovered.append(str(destination))
    return recovered


def summary_complete(work):
    path = work / "summary.json"
    if not path.exists():
        return False
    report = json.loads(path.read_text())
    if not report.get("status", "").startswith("complete"):
        return False
    if report.get("observed_runs") != report.get("planned_runs"):
        return False
    if report.get("validation_failures") != 0:
        return False
    return report["arms"]["jev"]["guide_fallbacks"] == 0


def command_for(slug):
    command = [
        "python3", str(RUNNER), "--work",
        str(ROOT / f".work_hdcv/fb15k_jev640_{slug}"), "--engine", "gpu",
        "--phase", "all",
    ]
    if slug == "random":
        command += ["--base-policy", "random"]
    else:
        command += [
            "--base-policy", "learned", "--value-model",
            str(ROOT / f".work_hdcv/fixpoint_q640/models/q_{slug}.meta"),
        ]
        if slug == "hdc":
            command += ["--hdc-value-backend", "original"]
    return command


def run_suite(slug):
    work = ROOT / f".work_hdcv/fb15k_jev640_{slug}"
    for attempt in range(1, 5):
        ensure_service()
        recovered = recover_fallbacks(work)
        write_status("running", suite=slug, attempt=attempt,
                     recovered_fallbacks=recovered)
        env = dict(os.environ, CUDA_VISIBLE_DEVICES="2")
        result = subprocess.run(command_for(slug), cwd=ROOT, env=env,
                                check=False)
        if result.returncode == 0 and summary_complete(work):
            write_status("suite_complete", suite=slug)
            return
        write_status("retrying", suite=slug, attempt=attempt,
                     returncode=result.returncode)
        time.sleep(15)
    raise RuntimeError(f"{slug} did not complete cleanly after four attempts")


def wait_for_external(slug, pid):
    work = ROOT / f".work_hdcv/fb15k_jev640_{slug}"
    write_status("waiting_for_existing_runner", suite=slug, pid=pid)
    while not summary_complete(work):
        ensure_service()
        if not pid_alive(pid):
            return
        time.sleep(15)


def validate_raw_runs(work):
    with (work / "goals.tsv").open() as stream:
        goals = list(csv.DictReader(stream, delimiter="\t"))
    expected = {(goal["goal_id"], arm, seed)
                for goal in goals for arm in ("baseline", "jev")
                for seed in (7, 13, 42)}
    for goal, arm, seed in expected:
        run = work / "runs" / goal / arm / f"seed{seed}"
        resource = json.loads((run / "resource.json").read_text())
        if resource["status"] != "completed":
            raise RuntimeError(f"incomplete run: {run}")
    return len(expected)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--numeric-pid", type=int)
    parser.add_argument("--existing-jev-pid", type=int)
    args = parser.parse_args()
    if args.existing_jev_pid is not None:
        SERVICE_PIDS.add(args.existing_jev_pid)
    try:
        write_status("queue_started", pid=os.getpid())
        ensure_service()
        if args.numeric_pid is not None:
            wait_for_external("numeric", args.numeric_pid)
        for slug in ("numeric", "random", "hdc"):
            run_suite(slug)
        subprocess.run(["python3", str(REPORT)], cwd=ROOT, check=True)
        counts = {slug: validate_raw_runs(
            ROOT / f".work_hdcv/fb15k_jev640_{slug}")
            for slug in ("numeric", "random", "hdc")}
        counts["deterministic"] = validate_raw_runs(
            ROOT / ".work_hdcv/fb15k_jev640")
        write_status("complete", runs=counts,
                     total_runs=sum(counts.values()),
                     report="experiments/fb15k-237-results/full-performance-report.md")
    except Exception as error:
        write_status("failed", error=str(error))
        raise
    finally:
        shutdown_services()
        if STATUS.exists():
            payload = json.loads(STATUS.read_text())
            payload["jev_service"] = "stopped"
            STATUS.write_text(json.dumps(payload, indent=2) + "\n")


if __name__ == "__main__":
    main()
