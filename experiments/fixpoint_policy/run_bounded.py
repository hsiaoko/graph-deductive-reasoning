#!/usr/bin/env python3
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import time


def process_tree(root):
    children = {}
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            fields = (entry / "stat").read_text().split()
            children.setdefault(int(fields[3]), []).append(int(entry.name))
        except (OSError, ValueError, IndexError):
            continue
    result = {root}
    queue = [root]
    while queue:
        for child in children.get(queue.pop(), []):
            if child not in result:
                result.add(child)
                queue.append(child)
    return result


def tree_rss(root):
    total = 0
    for pid in process_tree(root):
        try:
            for line in Path(f"/proc/{pid}/status").read_text().splitlines():
                if line.startswith("VmRSS:"):
                    total += int(line.split()[1]) * 1024
                    break
        except (OSError, ValueError, IndexError):
            pass
    return total


def system_used():
    values = {}
    for line in Path("/proc/meminfo").read_text().splitlines():
        key, value = line.split(":", 1)
        values[key] = int(value.split()[0]) * 1024
    return values["MemTotal"] - values["MemAvailable"]


def stop_group(pid):
    try:
        os.killpg(pid, signal.SIGTERM)
        time.sleep(2)
        os.killpg(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True)
    parser.add_argument("--result", required=True)
    parser.add_argument("--timeout-sec", type=float, default=1000)
    parser.add_argument("--memory-gb", type=float, default=256)
    parser.add_argument("--system-memory-gb", type=float, default=256)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("missing command")
    log_path = Path(args.log)
    result_path = Path(args.result)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    result_path.parent.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    status = "completed"
    returncode = None
    peak_rss = 0
    peak_system = system_used()
    with log_path.open("w") as log:
        proc = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT,
                                start_new_session=True)
        while True:
            returncode = proc.poll()
            elapsed = time.monotonic() - started
            if returncode is not None:
                peak_rss = max(peak_rss, tree_rss(proc.pid))
                break
            peak_rss = max(peak_rss, tree_rss(proc.pid))
            peak_system = max(peak_system, system_used())
            if elapsed >= args.timeout_sec:
                status = "timeout"
                stop_group(proc.pid)
                returncode = proc.wait()
                break
            if peak_rss >= args.memory_gb * (1 << 30):
                status = "oom_process_tree"
                stop_group(proc.pid)
                returncode = proc.wait()
                break
            if peak_system >= args.system_memory_gb * (1 << 30):
                status = "oom_system"
                stop_group(proc.pid)
                returncode = proc.wait()
                break
            time.sleep(0.2)
    result = {
        "command": command,
        "status": status,
        "returncode": returncode,
        "wall_sec": time.monotonic() - started,
        "peak_tree_rss_bytes": peak_rss,
        "peak_system_used_bytes": peak_system,
        "timeout_sec": args.timeout_sec,
        "memory_limit_bytes": int(args.memory_gb * (1 << 30)),
        "system_memory_limit_bytes": int(args.system_memory_gb * (1 << 30)),
    }
    result_path.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result))
    return 0 if status == "completed" and returncode == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
