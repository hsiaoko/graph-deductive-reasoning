#!/usr/bin/env python3
import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / ".work_hdcv/hdc_ablation6"
RUNS = WORK / "runs"
CSR = "/ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr"
RULES = "/ssd_data/zhuxk/graphs/KG/fb15k-237/rules"
BOUNDED = ROOT / "experiments/fixpoint_policy/run_bounded.py"
GOALS = [
    ("athletes", "anchor", "/olympics/olympic_participating_country/athletes./olympics/olympic_athlete_affiliation/olympics(/m/05r4w,/m/09n48)", "g0"),
    ("r2c_2", "anchor", "/location/hud_county_place/county(/m/0r540,/m/0l2rj)", "r2c_2"),
    ("r2c_4", "anchor", "/music/genre/artists(/m/0fd3y,/m/01vvycq)", "r2c_4"),
    ("r2c_8", "anchor", "/film/film/runtime./film/film_cut/film_release_region(/m/04v8x9,/m/02w7gg)", "r2c_8"),
    ("hard_f1", "hard_generalization", "/award/award_nominated_work/award_nominations./award/award_nomination/nominated_for(/m/014kq6,/m/01npcx)", "hard_pool"),
    ("hard_f2", "hard_generalization", "/award/award_nominated_work/award_nominations./award/award_nomination/nominated_for(/m/014kq6,/m/025twgf)", "hard_pool"),
]


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def models(goal_id, model_id):
    if model_id == "hard_pool":
        base = ROOT / ".work_hdcv/hdc_hard_goals/models"
        return base / "hard_pool.meta", base / "hard_pool_num.meta"
    return (ROOT / f".work_hdcv/models/{model_id}.meta",
            ROOT / f".work_hdcv/hdc_hard_goals/models/{model_id}.meta")


def freeze():
    WORK.mkdir(parents=True, exist_ok=True)
    with (WORK / "goals.tsv").open("w") as stream:
        writer = csv.writer(stream, delimiter="\t", lineterminator="\n")
        writer.writerow(["goal_id", "split", "spec", "model_id"])
        writer.writerows(GOALS)
    manifest = {"binary_sha256": sha256(ROOT / "build/bin/gar_reason"),
                "goals": [], "planned_runs": 18}
    for goal_id, split, spec, model_id in GOALS:
        hdc, numeric = models(goal_id, model_id)
        manifest["goals"].append({
            "goal_id": goal_id, "split": split, "spec": spec,
            "hdc_model": str(hdc), "hdc_sha256": sha256(hdc),
            "hdc_weights_sha256": sha256(hdc.with_suffix(".bin")),
            "numeric_model": str(numeric), "numeric_sha256": sha256(numeric),
            "numeric_weights_sha256": sha256(numeric.with_suffix(".bin")),
        })
    (WORK / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


def run(goal_id, spec, arm, command):
    directory = RUNS / goal_id / arm
    result = directory / "resource.json"
    if result.exists():
        saved = json.loads(result.read_text())
        print(f"skip {goal_id}/{arm}: {saved['status']}", flush=True)
        return
    directory.mkdir(parents=True, exist_ok=True)
    bounded = ["python3", str(BOUNDED), "--log", str(directory / "run.log"),
               "--result", str(result), "--timeout-sec", "1000",
               "--memory-gb", "256", "--system-memory-gb", "256", "--",
               *map(str, command)]
    print(f"run {goal_id}/{arm}", flush=True)
    env = dict(os.environ, GAR_DEBUG_BNB="1")
    subprocess.run(bounded, cwd=ROOT, check=False, env=env)
    saved = json.loads(result.read_text())
    if saved["status"].startswith("oom"):
        raise RuntimeError(f"stop after {goal_id}/{arm}: {saved['status']}")


def main():
    freeze()
    with (WORK / "goals.tsv").open() as stream:
        goals = list(csv.DictReader(stream, delimiter="\t"))
    for row in goals:
        goal_id, spec, model_id = row["goal_id"], row["spec"], row["model_id"]
        hdc, numeric = models(goal_id, model_id)
        base = ["./build/bin/gar_reason", "--mode", "agent", "--csr", CSR,
                "--rules", RULES, "--goal", spec, "--executor", "bnb",
                "--budget", "2", "--max-candidates-per-rule", "64", "--top-k",
                "0", "--policy", "hybrid", "--scorer", "exact", "--print", "0"]
        for arm, priority, model in (("utility", "utility", None),
                                     ("numeric", "numeric-value", numeric),
                                     ("hdc", "hdc-value", hdc)):
            command = base + ["--out", RUNS / goal_id / arm,
                              "--bnb-priority", priority]
            if model is not None:
                command += ["--value-model", str(model)]
            run(goal_id, spec, arm, command)


if __name__ == "__main__":
    main()
