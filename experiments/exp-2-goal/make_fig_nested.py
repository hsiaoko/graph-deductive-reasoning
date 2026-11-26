#!/usr/bin/env python3
"""Nested rule-set scaling figure: Chase upper bound vs AdaDeduce vs goal lower
bound, log-log axes. Distinct markers + line styles + direct labels (secondary
encoding for colorblind safety)."""
import json, math
from pathlib import Path

WORK = Path(__file__).resolve().parent / "work_nested"
data = json.loads((WORK / "results_fixed.json").read_text())

W, H = 780, 500
L, R, T, B = 82, 660, 80, 400
xs = [d["rules"] for d in data]
chase = [d["chase_closure"] for d in data]
dep = [d["dep_dedup_union"] for d in data]
det = [d["det_dedup_union"] for d in data]
ada = [d["ada_dedup_union"] for d in data]
goals = [d["goals"] for d in data]

xmin, xmax = 8.0, 520.0
ymin, ymax = 20.0, 2_000_000.0
lx0, lx1 = math.log10(xmin), math.log10(xmax)
ly0, ly1 = math.log10(ymin), math.log10(ymax)


def sx(v):
    return L + (math.log10(v) - lx0) / (lx1 - lx0) * (R - L)


def sy(v):
    return B - (math.log10(v) - ly0) / (ly1 - ly0) * (B - T)


BLUE, PURPLE, AMBER, GREEN, GRAY = ("#1d4ed8", "#7c3aed", "#b45309",
                                    "#166534", "#6b7280")
INK, GRID, MUT = "#1f2937", "#e5e7eb", "#6b7280"
s = []
s.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
         f'viewBox="0 0 {W} {H}" font-family="Helvetica,Arial,sans-serif">')
s.append(f'<rect width="{W}" height="{H}" fill="#ffffff"/>')
s.append(f'<text x="{L}" y="34" font-size="18" font-weight="700" fill="{INK}">'
         'AdaDeduce derives near-minimal facts as the rule set grows</text>')
s.append(f'<text x="{L}" y="56" font-size="13" fill="{MUT}">'
         'BioKG · nested random rule sets · 50 goals each · dedup counts · '
         'log–log axes</text>')
# y gridlines (log decades)
for yv in [100, 1000, 10000, 100000, 1000000]:
    y = sy(yv)
    s.append(f'<line x1="{L}" y1="{y:.1f}" x2="{R}" y2="{y:.1f}" '
             f'stroke="{GRID}" stroke-width="1"/>')
    lab = {100: "100", 1000: "1K", 10000: "10K", 100000: "100K",
           1000000: "1M"}[yv]
    s.append(f'<text x="{L-10}" y="{y+4:.1f}" font-size="12" fill="{MUT}" '
             f'text-anchor="end">{lab}</text>')
# x ticks
for xv in [10, 50, 100, 200, 400]:
    x = sx(xv)
    s.append(f'<line x1="{x:.1f}" y1="{B}" x2="{x:.1f}" y2="{B+5}" '
             f'stroke="{MUT}" stroke-width="1"/>')
    s.append(f'<text x="{x:.1f}" y="{B+20}" font-size="12" fill="{MUT}" '
             f'text-anchor="middle">{xv}</text>')
s.append(f'<line x1="{L}" y1="{B}" x2="{R}" y2="{B}" stroke="{INK}" '
         'stroke-width="1.5"/>')
s.append(f'<line x1="{L}" y1="{T}" x2="{L}" y2="{B}" stroke="{INK}" '
         'stroke-width="1.5"/>')
s.append(f'<text x="{(L+R)/2:.0f}" y="{B+44}" font-size="13" fill="{INK}" '
         'text-anchor="middle">Number of GARs (nested rule set)</text>')
s.append(f'<text x="24" y="{(T+B)/2:.0f}" font-size="13" fill="{INK}" '
         'text-anchor="middle" transform="rotate(-90 24 '
         f'{(T+B)/2:.0f})">Facts derived (dedup, log scale)</text>')


def polyline(vals, color, dash=""):
    pts = " ".join(f"{sx(x):.1f},{sy(y):.1f}" for x, y in zip(xs, vals))
    d = f' stroke-dasharray="{dash}"' if dash else ""
    s.append(f'<polyline points="{pts}" fill="none" stroke="{color}" '
             f'stroke-width="2.5"{d}/>')


# goals lower bound (gray dashed)
polyline(goals, GRAY, "6,4")
# chase upper (blue solid, circles)
polyline(chase, BLUE)
for x, y in zip(xs, chase):
    s.append(f'<circle cx="{sx(x):.1f}" cy="{sy(y):.1f}" r="5" fill="{BLUE}"/>')
# dep-only cone chase (purple solid, diamonds)
polyline(dep, PURPLE)
for x, y in zip(xs, dep):
    cx, cy = sx(x), sy(y)
    s.append(f'<polygon points="{cx:.1f},{cy-6:.1f} {cx+6:.1f},{cy:.1f} '
             f'{cx:.1f},{cy+6:.1f} {cx-6:.1f},{cy:.1f}" fill="{PURPLE}"/>')
# det forward (amber solid, triangles)
polyline(det, AMBER)
for x, y in zip(xs, det):
    cx, cy = sx(x), sy(y)
    s.append(f'<polygon points="{cx:.1f},{cy-6:.1f} {cx-6:.1f},{cy+5:.1f} '
             f'{cx+6:.1f},{cy+5:.1f}" fill="{AMBER}"/>')
# ada (green solid, squares)
polyline(ada, GREEN)
for x, y in zip(xs, ada):
    cx, cy = sx(x), sy(y)
    s.append(f'<rect x="{cx-5:.1f}" y="{cy-5:.1f}" width="10" height="10" '
             f'fill="{GREEN}"/>')
# right-side legend (mid-right, clear of the top/bottom data clusters)
lx, ly = 636, 168
s.append(f'<rect x="{lx-8}" y="{ly-16}" width="148" height="128" rx="6" '
         f'fill="#ffffff" stroke="{GRID}" stroke-width="1"/>')
legend = [(BLUE, "Chase", f"{chase[-1]:,}", "circle"),
          (PURPLE, "DepOnly", f"{dep[-1]:,}", "diamond"),
          (AMBER, "Det fwd", f"{det[-1]:,}", "tri"),
          (GREEN, "AdaDeduce", f"{ada[-1]}", "sq"),
          (GRAY, "goals", f"{goals[-1]}", "dash")]
for i, (col, name, val, mark) in enumerate(legend):
    yy = ly + i * 24
    if mark == "circle":
        s.append(f'<circle cx="{lx+2}" cy="{yy-4:.1f}" r="5" fill="{col}"/>')
    elif mark == "diamond":
        s.append(f'<polygon points="{lx+2},{yy-9:.1f} {lx+7},{yy-4:.1f} '
                 f'{lx+2},{yy+1:.1f} {lx-3},{yy-4:.1f}" fill="{col}"/>')
    elif mark == "tri":
        s.append(f'<polygon points="{lx+2},{yy-9:.1f} {lx-3},{yy+1:.1f} '
                 f'{lx+7},{yy+1:.1f}" fill="{col}"/>')
    elif mark == "sq":
        s.append(f'<rect x="{lx-3}" y="{yy-9:.1f}" width="10" height="10" '
                 f'fill="{col}"/>')
    else:
        s.append(f'<line x1="{lx-4}" y1="{yy-4:.1f}" x2="{lx+8}" y2="{yy-4:.1f}" '
                 f'stroke="{col}" stroke-width="2.5" stroke-dasharray="5,3"/>')
    s.append(f'<text x="{lx+16}" y="{yy:.1f}" font-size="11.5" fill="{INK}">'
             f'{name}</text>')
    s.append(f'<text x="{lx+132}" y="{yy:.1f}" font-size="11.5" '
             f'font-weight="700" fill="{col}" text-anchor="end">{val}</text>')
# annotation
ratio = chase[-1] / ada[-1]
s.append(f'<rect x="{L+14}" y="{T+12}" width="270" height="42" rx="6" '
         f'fill="#eff6ff" stroke="{BLUE}" stroke-width="1"/>')
s.append(f'<text x="{L+26}" y="{T+30}" font-size="12.5" font-weight="700" '
         f'fill="{BLUE}">{ratio:,.0f}× fewer facts than Chase</text>')
s.append(f'<text x="{L+26}" y="{T+46}" font-size="11.5" fill="{INK}">'
         'at 400 rules · AdaDeduce tracks the goal lower bound</text>')
s.append('</svg>')
(WORK / "fig_nested.svg").write_text("\n".join(s))
print(f"wrote {WORK / 'fig_nested.svg'}")