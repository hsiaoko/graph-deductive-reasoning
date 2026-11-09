#!/usr/bin/env python3
import argparse
import csv
import hashlib
import json
from pathlib import Path
import sys

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / ".work_hdcv/fixpoint_q640"
NAMES = ["price", "pattern_edges", "log_support", "relfam", "relevd", "conf",
         "dcost", "total", "dp0", "wit", "premises"]
CONJ = [(0, 4), (2, 6), (5, 7), (8, 9)]
D = 4096
BIN_CAP, BIN_SCALE, LAM = 7, 2.0, 1.0


def vector(key):
    digest = hashlib.sha256(f"fixpoint-q-v1|{key}".encode()).digest()
    rng = np.random.default_rng(int.from_bytes(digest[:4], "little"))
    return rng.choice([-1.0, 1.0], size=D)


def hdc_features(raw, base_vectors):
    bins = [min(BIN_CAP, max(0, int(np.floor(abs(x) * BIN_SCALE))))
            for x in raw]
    keyed = [base_vectors[j * (BIN_CAP + 1) + bins[j]]
             for j in range(len(NAMES))]
    hv = np.zeros(D)
    for value in keyed:
        hv += value
    for a, b in CONJ:
        hv += keyed[a] * keyed[b]
    hv += base_vectors[-1]
    return hv


def ridge(X, y):
    X1 = np.hstack([X, np.ones((len(X), 1))])
    return np.linalg.solve(X1.T @ X1 + LAM * np.eye(X1.shape[1]), X1.T @ y)


def read_trace(path):
    with path.open() as stream:
        reader = csv.DictReader(stream, delimiter="\t")
        rows = []
        for row in reader:
            if int(row["goal_derived"]) != 1:
                continue
            y = float(row["remaining_evaluations"])
            if y <= 0:
                continue
            rows.append(([float(row[name]) for name in NAMES], y))
    if not rows:
        return np.empty((0, len(NAMES))), np.empty(0)
    rows.sort(key=lambda item: item[1])
    indices = np.linspace(0, len(rows) - 1, min(256, len(rows)), dtype=int)
    selected = [rows[index] for index in indices]
    return (np.asarray([item[0] for item in selected]),
            np.asarray([item[1] for item in selected]))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--traces", type=Path, default=WORK / "traces")
    parser.add_argument("--out", type=Path, default=WORK / "models")
    parser.add_argument("--test-goals", type=Path,
                        default=ROOT / ".work_hdcv/fixpoint_rules640/goals.tsv")
    args = parser.parse_args()
    with args.test_goals.open() as stream:
        test_specs = {row["spec"] for row in csv.DictReader(stream, delimiter="\t")}
    Xs, ys, trained_on, failed = [], [], [], []
    for path in sorted(args.traces.glob("*/*/q_trace.tsv")):
        run = path.parent
        resource_path = run / "resource.json"
        command = json.loads(resource_path.read_text())["command"] if resource_path.exists() else []
        goal_spec = command[command.index("--goal") + 1] if "--goal" in command else ""
        if goal_spec in test_specs:
            raise RuntimeError(f"test goal leaked into training: {goal_spec}")
        X, y = read_trace(path)
        if len(y) == 0:
            failed.append(str(path.relative_to(args.traces)))
            continue
        Xs.append(X)
        ys.append(y)
        trained_on.append(str(path.relative_to(args.traces)))
    if not Xs:
        raise RuntimeError("no successful training traces")
    X = np.vstack(Xs)
    y = np.concatenate(ys)
    args.out.mkdir(parents=True, exist_ok=True)
    keys = [(name, b) for name in NAMES for b in range(BIN_CAP + 1)] + [("bias", 0)]
    base_vectors = np.stack([vector(key) for key in keys]).astype(np.int8)
    Xh = np.stack([hdc_features(row, base_vectors) for row in X])
    wh = ridge(Xh, y)
    trained_text = " ".join(trained_on)
    with (args.out / "q_hdc.meta").open("w") as stream:
        stream.write("format hdc_value_v1\n")
        stream.write(f"dim {D}\nn_keys {len(keys)}\nbin_cap {BIN_CAP}\n")
        stream.write(f"bin_scale {BIN_SCALE}\nn_features {len(NAMES)}\n")
        stream.write("names " + " ".join(NAMES) + "\n")
        for index, (name, bin_id) in enumerate(keys):
            stream.write(f"key {index} {name} {bin_id}\n")
        for a, b in CONJ:
            stream.write(f"conj {a} {b}\n")
        stream.write(f"lam {LAM}\ntarget remaining_evaluations\n")
        stream.write(f"trained_on {trained_text}\ntrain_nodes {len(y)}\n")
    with (args.out / "q_hdc.bin").open("wb") as stream:
        stream.write(base_vectors.tobytes())
        stream.write(wh.astype(np.float64).tobytes())
    mu = X.mean(axis=0)
    sd = X.std(axis=0)
    sd[sd == 0] = 1.0
    Z = (X - mu) / sd
    F = np.hstack([Z] + [(Z[:, a] * Z[:, b])[:, None] for a, b in CONJ])
    wn = ridge(F, y)
    with (args.out / "q_numeric.meta").open("w") as stream:
        stream.write("format numeric_ridge_v1\n")
        stream.write(f"n_features {len(NAMES)}\n")
        stream.write("names " + " ".join(NAMES) + "\n")
        for a, b in CONJ:
            stream.write(f"conj {a} {b}\n")
        stream.write("mean " + " ".join(f"{v:.17g}" for v in mu) + "\n")
        stream.write("std " + " ".join(f"{v:.17g}" for v in sd) + "\n")
        stream.write(f"lam {LAM}\ntarget remaining_evaluations\n")
        stream.write(f"trained_on {trained_text}\ntrain_nodes {len(y)}\n")
    with (args.out / "q_numeric.bin").open("wb") as stream:
        stream.write(np.asarray(wn, dtype=np.float64).tobytes())
    manifest = {"train_nodes": len(y), "successful_traces": trained_on,
                "failed_or_empty_traces": failed, "test_goal_count": len(test_specs),
                "feature_names": NAMES, "conjunctions": CONJ, "hdc_dim": D,
                "sampling": "sort by remaining_evaluations; at most 256 evenly spaced rows/trace"}
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"exported {len(y)} rows from {len(trained_on)} traces; failed/empty={len(failed)}", file=sys.stderr)


if __name__ == "__main__":
    main()
