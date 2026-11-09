#!/usr/bin/env python3
"""Cross-check C++ ValueModel numeric predictions against the Python
reference (same math as train_numeric.py's export). Usage:
  verify_numeric.py <model.meta>
Prints max |diff| over sample rows; exits 1 if > 1e-9.
"""
import subprocess, sys
import numpy as np

def load_meta(path):
    meta = {'conj': []}
    for line in open(path):
        t = line.split()
        if not t: continue
        if t[0] == 'names': meta['names'] = t[1:]
        elif t[0] == 'conj': meta['conj'].append((int(t[1]), int(t[2])))
        elif t[0] == 'mean': meta['mean'] = [float(x) for x in t[1:]]
        elif t[0] == 'std': meta['std'] = [float(x) for x in t[1:]]
    return meta

def main(path):
    meta = load_meta(path)
    w = np.fromfile(path.replace('.meta', '.bin'), dtype=np.float64)
    nf = len(meta['names'])
    assert len(w) == nf + len(meta['conj']) + 1, (len(w), nf)
    rng = np.random.default_rng(0)
    rows = np.concatenate([
        np.zeros((1, nf)),
        np.ones((1, nf)),
        rng.uniform(0, 5, size=(50, nf)),
        np.abs(rng.normal(0, 50, size=(50, nf))),
    ])
    mu, sd = np.array(meta['mean']), np.array(meta['std'])
    Z = (rows - mu) / sd
    F = np.hstack([Z] + [(Z[:, a] * Z[:, b])[:, None]
                         for a, b in meta['conj']])
    ref = F @ w[:-1] + w[-1]
    inp = "\n".join(" ".join(f"{v:.17g}" for v in r) for r in rows) + "\n"
    out = subprocess.run(["./build/bin/value_model_check", path],
                         input=inp, capture_output=True, text=True)
    got = np.array([float(x) for x in out.stdout.split()])
    diff = np.max(np.abs(got - ref))
    print(f"{path}: rows={len(rows)} max|diff|={diff:.3e}")
    if diff > 1e-9:
        sys.exit(1)

if __name__ == '__main__':
    main(sys.argv[1])
