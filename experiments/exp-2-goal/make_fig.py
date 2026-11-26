#!/usr/bin/env python3
"""Figure (a): candidate-action reduction vs goal-cone size, zero goal loss."""
import json
from pathlib import Path

WORK = Path(__file__).resolve().parent / "work_biokg"
data = json.loads((WORK / "results.json").read_text())
pts = data["panel"]

W, H = 760, 480
L, R, T, B = 78, 700, 78, 388  # plot box
xmin, xmax = 0, 85
ymin, ymax = 97.5, 100.3


def sx(v):
    return L + (v - xmin) / (xmax - xmin) * (R - L)


def sy(v):
    return B - (v - ymin) / (ymax - ymin) * (B - T)


BLUE = "#1d4ed8"
AMBER = "#b45309"
INK = "#1f2937"
GRID = "#e5e7eb"
MUT = "#6b7280"

s = []
s.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
         f'viewBox="0 0 {W} {H}" font-family="Helvetica,Arial,sans-serif">')
s.append(f'<rect width="{W}" height="{H}" fill="#ffffff"/>')
s.append(f'<text x="{L}" y="34" font-size="18" font-weight="700" fill="{INK}">'
         'Relevance filtering removes 98–100% of candidate actions</text>')
s.append(f'<text x="{L}" y="56" font-size="13" fill="{MUT}">'
         'BioKG · 720 GARs · single-thread · safe filtering: 0 / 5 derivable goals '
         'lost</text>')
# gridlines + y ticks
for yv in [98, 99, 100]:
    y = sy(yv)
    s.append(f'<line x1="{L}" y1="{y:.1f}" x2="{R}" y2="{y:.1f}" '
             f'stroke="{GRID}" stroke-width="1"/>')
    s.append(f'<text x="{L-10}" y="{y+4:.1f}" font-size="12" fill="{MUT}" '
             f'text-anchor="end">{yv}%</text>')
# x ticks
for xv in [0, 20, 40, 60, 80]:
    x = sx(xv)
    s.append(f'<line x1="{x:.1f}" y1="{B}" x2="{x:.1f}" y2="{B+5}" '
             f'stroke="{MUT}" stroke-width="1"/>')
    s.append(f'<text x="{x:.1f}" y="{B+20}" font-size="12" fill="{MUT}" '
             f'text-anchor="middle">{xv}</text>')
# axes
s.append(f'<line x1="{L}" y1="{B}" x2="{R}" y2="{B}" stroke="{INK}" '
         'stroke-width="1.5"/>')
s.append(f'<line x1="{L}" y1="{T}" x2="{L}" y2="{B}" stroke="{INK}" '
         'stroke-width="1.5"/>')
s.append(f'<text x="{(L+R)/2:.0f}" y="{B+44}" font-size="13" fill="{INK}" '
         'text-anchor="middle">Goal-cone size (number of relevant GARs)</text>')
s.append(f'<text x="22" y="{(T+B)/2:.0f}" font-size="13" fill="{INK}" '
         'text-anchor="middle" transform="rotate(-90 22 '
         f'{(T+B)/2:.0f})">Candidate actions removed (%)</text>')
# points + direct labels
short = {
    "drug-drug_irritable_bowel_syndrome": "irritable bowel (cone 4)",
    "drug-drug_inherited_metabolic_disorder": "metabolic (cone 41)",
    "drug-drug_viral_infectious_disease": "viral (cone 43)",
    "drug-drug_immune_system_disease": "immune (cone 46)",
    "drug-drug_respiratory_system_disease": "respiratory (cone 79)",
}
for p in pts:
    x = sx(p["cone_size"]); y = sy(p["candidate_reduction_pct"])
    s.append(f'<circle cx="{x:.1f}" cy="{y:.1f}" r="6" fill="{BLUE}"/>')
    lab = short.get(p["predicate"], p["predicate"])
    s.append(f'<text x="{x:.1f}" y="{y-12:.1f}" font-size="11.5" fill="{INK}" '
             f'text-anchor="middle">{lab} · {p["candidate_reduction_pct"]:.1f}%</text>')
# safe-filtering annotation (top-right, clear of data marks)
ax = R - 262
s.append(f'<rect x="{ax}" y="{T+10}" width="250" height="40" rx="6" '
         f'fill="#fff7ed" stroke="{AMBER}" stroke-width="1"/>')
s.append(f'<text x="{ax+12}" y="{T+27}" font-size="12" font-weight="700" '
         f'fill="{AMBER}">Safe filtering verified</text>')
s.append(f'<text x="{ax+12}" y="{T+43}" font-size="11.5" fill="{INK}">'
         '0 derivable goals lost at every point</text>')
s.append('</svg>')
(WORK / "fig_a.svg").write_text("\n".join(s))
print(f"wrote {WORK / 'fig_a.svg'}")