#!/usr/bin/env python3
import hashlib
import json
from pathlib import Path
import statistics

ROOT = Path(__file__).resolve().parents[2]
OLD = json.loads((ROOT / ".work_hdcv/fixpoint_unified640/summary.json").read_text())
NEW = json.loads((ROOT / ".work_hdcv/fb15k_jev640/summary.json").read_text())
RULES = json.loads((ROOT / ".work_hdcv/fixpoint_rules640/summary.json").read_text())
COLD = json.loads((ROOT / ".work_hdcv/fb15k_jev640_cold_p00/summary.json").read_text())
SUITES = []
for slug, label in (("numeric", "numeric-Q"), ("random", "Random"),
                    ("hdc", "HDC-Q")):
    path = ROOT / f".work_hdcv/fb15k_jev640_{slug}/summary.json"
    if path.exists():
        SUITES.append({"slug": slug, "label": label, "path": path,
                       "data": json.loads(path.read_text())})
CPU_PATH = ROOT / ".work_hdcv/adaptive_fb15k_cpu_fixed/summary.json"
CPU = json.loads(CPU_PATH.read_text()) if CPU_PATH.exists() else None
OUTPUT = ROOT / "experiments/fb15k-237-results/full-performance-report.md"

OLD_LABELS = {
    "goal_chase": "GoalChase / ChaseDepOnly",
    "deterministic": "Deterministic",
    "random": "Random",
    "q_numeric": "AdaDeduce / Learned numeric-Q",
    "q_hdc": "AdaDeduce / Learned HDC-Q",
}


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def old_goal(arm):
    return {row["goal"]: row for row in OLD["arms"][arm]["per_goal"]}


def new_goal(arm):
    return {row["goal"]: row for row in NEW["arms"][arm]["per_goal"]}


def milliseconds(path, key):
    for line in path.read_text().splitlines():
        if line.startswith(key + ":"):
            return float(line.split(":", 1)[1]) / 1000.0
    raise KeyError(f"{key} not found in {path}")


OLD_D = old_goal("deterministic")
NEW_D = new_goal("baseline")
D_WALL_DIFF = (NEW["arms"]["baseline"]["macro"]["wall"] -
               OLD["arms"]["deterministic"]["macro"]["wall"])
D_REGRESSIONS = sorted(
    (NEW_D[goal]["wall"] - row["wall"] for goal, row in OLD_D.items()),
    reverse=True)
D_TOP2_SHARE = sum(D_REGRESSIONS[:2]) / 36 / D_WALL_DIFF
D_P21_OLD_DELTA = milliseconds(
    ROOT / ".work_hdcv/fixpoint_unified640/runs/p21/deterministic/seed7/summary.yaml",
    "fx_delta_wall_ms")
D_P21_NEW_DELTA = milliseconds(
    ROOT / ".work_hdcv/fb15k_jev640/runs/p21/baseline/seed7/summary.yaml",
    "delta_wall_ms")
D_P34_OLD_DELTA = milliseconds(
    ROOT / ".work_hdcv/fixpoint_unified640/runs/p34/deterministic/seed7/summary.yaml",
    "fx_delta_wall_ms")
D_P34_NEW_DELTA = milliseconds(
    ROOT / ".work_hdcv/fb15k_jev640/runs/p34/baseline/seed7/summary.yaml",
    "delta_wall_ms")


lines = [
    "# FB15K-237 完整实验报告：Chase、GoalChase、Deterministic、Random、AdaDeduce 与 Jev",
    "",
    "## 1. 结论摘要",
    "",
    "本报告汇总两组已经完成的实验，避免遗漏基线，也避免把不同实现的墙钟混为同一排名。",
    "",
    "1. 历史统一五方法矩阵共 540/540 runs，全部命中且 soundness 通过。numeric-Q 的宏平均外部 wall 最低（7.375s），Deterministic 为 7.554s，GoalChase 为 7.892s，Random 为 8.036s，原始 HDC-Q 为 9.192s。",
    "2. 当前 GPU Deterministic/Jev 矩阵共 216/216 runs，全部命中且 soundness 通过。Jev 外部 wall 从 11.035s 增至 11.187s，求值从 46,637.1 增至 46,649.6，没有整体端到端收益。",
    "3. 完整 Chase 在 rules640 上派生 5,019,342 条事实，三次 cold wall 中位数为 742.462s。它计算完整闭包，agent 在每个 goal 派生后停止，二者不是相同输出规模下的等价工作量。",
    "4. 历史五方法使用旧 CPU Fixpoint 实现；当前 Jev 对照使用当前 GPU AdaDeduce。两组结果分别成立，不能用 Jev 的 11.187s 直接对历史 numeric-Q 的 7.375s 宣称方法优劣。",
    "",
    "## 2. 方法名称与精确定义",
    "",
    "| 报告名称 | 当前代码/论文含义 | 选择机制与停止条件 |",
    "|---|---|---|",
    "| Chase | `--mode chase` | 全部规则按固定顺序计算完整闭包，不在单个 goal 处停止 |",
    "| ChaseDepOnly / GoalChase | `--mode agent --policy deterministic --scorer null` | 只保留 goal-relevant 规则，不使用证据或成本排序，goal 派生后停止 |",
    "| Deterministic | `--policy deterministic --scorer exact` | AdaDeduce 执行器，选择精确组合分数最高的 ready action |",
    "| Random | `--policy random --scorer exact` | AdaDeduce 执行器，按 score/tau 的 softmax 分布采样 |",
    "| AdaDeduce / Learned | `--policy learned --scorer exact` | 论文实验默认的 AdaDeduce 变体，选择预测剩余成本最低的 action；本报告同时列 numeric-Q 与 HDC-Q |",
    "| Deterministic + local Jev | `--policy deterministic --llm-guide --llm-backend jev` | 起步时最多调用一次模型重排 star，之后仍由 Deterministic policy 和精确 AdaDeduce 执行 |",
    "",
    "AdaDeduce 是执行算法，不是与 Deterministic、Random 完全同层的第四种 action policy。论文 `experimental_settings.tex` 约定实验表中的 AdaDeduce 默认指 Learned 变体，本报告保留该约定并明确模型类型。ChaseDepOnly 对应论文的 GoalChase，不是计算 goal-cone 完整闭包的另一种全量 Chase 口径。",
    "",
    "## 3. 数据、规则、goals 与 ground truth",
    "",
    "- 图：`/ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr`，14,513 个顶点、237 个关系、289,650 条输入边。",
    "- 规则：`.work_hdcv/fixpoint_rules640/rules640`，冻结的 640 条规则。",
    "- 测试 goals：`.work_hdcv/fixpoint_rules640/goals.tsv`，共 36 个固定实例 goal；round1、round2、deep 各 12 个。完整实体常量和 spec 以该文件为准。",
    "- Ground truth：`.work_hdcv/fixpoint_rules640/runs/chase/rep1`，28 轮到达 fixpoint，派生 5,019,342 条事实。",
    "- 共同 agent 参数：goal star order、budget=1,000,000,000、candidate cap=100,000、fair interval=0、print=0、内部/外部上限 900s/1000s。",
    "- 所有 goal 都是预期可派生的正例；hit rate 是正例命中率，不是正负例分类准确率。",
    "",
    "### Learned 模型的训练隔离",
    "",
    "训练 goal 从完整闭包中排除 36 个测试 goal 后冻结，round1、round2、deep 各 4 个，共 12 个。训练与测试 spec 不重叠。24 个 continuation trace 中 23 个命中；`t11/deterministic` 超时，不赋予人工有限成本标签。两种模型共同使用 5,757 个采样训练行。",
    "",
    "两种模型使用相同 11 个原始特征及 4 个交互项，预测从当前 action（含）到 goal 的剩余求值次数。numeric-Q 使用标准化数值 ridge；HDC-Q 使用 4096 维分箱双极随机特征及绑定，ridge lambda=1.0。未观察到的 alternative action 不伪造标签。",
    "",
    "## 4. 完整 Chase 参照",
    "",
    "| run | wall(s) | 全轮完整匹配 | 去重前派生候选 | 派生 | rounds | fixpoint |",
    "|---|---:|---:|---:|---:|---:|---|",
]
for index, row in enumerate(RULES["chase"], 1):
    lines.append(
        f"| {index} | {row['wall']:.3f} | {row['matches']:,} | {row['candidates']:,} | "
        f"{row['derived']:,} | {row['rounds']} | {str(row['fixpoint']).lower()} |")
chase_median = statistics.median(row["wall"] for row in RULES["chase"])
lines += [
    "",
    f"Chase 中位 wall 为 {chase_median:.3f}s，按 36 goals 做纯时间摊销参考为 {chase_median / 36:.3f}s/goal。这个摊销不代表每个 goal 的实际独立运行成本，也不能与 goal-directed 输出量直接等同。",
    "",
    "## 5. 历史统一五方法矩阵（540 runs）",
    "",
    "协议：`experiments/fixpoint_policy/UNIFIED640_PREREGISTRATION.md`。五种方法均执行 36 goals × seeds `{7,13,42}`，每臂 108 runs；逐 goal 取三 run 中位数，再做 36-goal 宏平均。该矩阵使用旧 CPU Fixpoint 实现。",
    "",
    "| 方法 | hit | soundness | 外部 wall(s) | 内部 wall(s) | 选择分支 | ready 候选 | 完整匹配 | 求值/派生 | 36-goal 代表 wall 合计(s) | cap | timeout |",
    "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
]
for arm, label in OLD_LABELS.items():
    item = OLD["arms"][arm]
    macro = item["macro"]
    lines.append(
        f"| {label} | {item['goal_hit_rate']:.3f} | {item['soundness_rate']:.3f} | "
        f"{macro['wall']:.3f} | {macro['internal_wall']:.3f} | {macro['branches']:,.1f} | "
        f"{macro['ready']:,.1f} | {macro['matches']:,.1f} | {macro['evaluations']:,.1f} | "
        f"{item['batch_wall']:.3f} | {item['total_caps']} | {item['timeout_runs']} |")
lines += [
    "",
    "选择分支包含零成本跳过已知结论；真实求值单列，二者不能混用。外部 wall 包含加载和写盘，内部 wall 只包围 agent 执行。",
    "",
    "### 相对历史 Deterministic",
    "",
    "| 方法 | 外部 wall 加速 | 内部 wall 加速 | 选择分支减少 | 真实求值减少 |",
    "|---|---:|---:|---:|---:|",
]
reference = OLD["arms"]["deterministic"]["macro"]
for arm, label in OLD_LABELS.items():
    if arm == "deterministic":
        continue
    macro = OLD["arms"][arm]["macro"]
    lines.append(
        f"| {label} | {reference['wall'] / macro['wall']:.3f}x | "
        f"{reference['internal_wall'] / macro['internal_wall']:.3f}x | "
        f"{100 * (1 - macro['branches'] / reference['branches']):.3f}% | "
        f"{100 * (1 - macro['evaluations'] / reference['evaluations']):.3f}% |")
lines += [
    "",
    "历史矩阵中，numeric-Q 取得最低平均 wall 和真实求值数，但相对 Deterministic 的墙钟收益只有约 2–3%。Random 整体慢于 Deterministic。原始 HDC-Q 的模型计算开销高，且没有减少真实求值，因此最慢。详细 HDC 成本分解见 `experiments/fb15k-237-results/overall-performance.md`。",
    "",
    "### 为什么两组 Deterministic 的 wall 差异很大",
    "",
    "两组都名为 Deterministic，但历史组使用旧 CPU Fixpoint，当前组使用重构后的 GPU AdaDeduce。它们不是只更换一个 CLI 参数的同一二进制，不能直接做严格的政策等价比价。",
    "",
    "| 指标（宏平均） | 历史 CPU Deterministic | 当前 GPU Deterministic | 差值 |",
    "|---|---:|---:|---:|",
    f"| 外部 wall(s) | {OLD['arms']['deterministic']['macro']['wall']:.3f} | {NEW['arms']['baseline']['macro']['wall']:.3f} | {D_WALL_DIFF:.3f} |",
    f"| 内部 wall(s) | {OLD['arms']['deterministic']['macro']['internal_wall']:.3f} | {NEW['arms']['baseline']['macro']['internal_wall']:.3f} | {NEW['arms']['baseline']['macro']['internal_wall'] - OLD['arms']['deterministic']['macro']['internal_wall']:.3f} |",
    f"| 求值 | {OLD['arms']['deterministic']['macro']['evaluations']:,.1f} | {NEW['arms']['baseline']['macro']['evaluations']:,.1f} | {NEW['arms']['baseline']['macro']['evaluations'] - OLD['arms']['deterministic']['macro']['evaluations']:,.1f} |",
    "",
    f"外部 wall 总差值为 {D_WALL_DIFF:.3f}s，其中内部执行差值为 {NEW['arms']['baseline']['macro']['internal_wall'] - OLD['arms']['deterministic']['macro']['internal_wall']:.3f}s，说明主要差异发生在算法执行内部，而非仅由 CUDA 初始化或进程加载造成。p21 和 p34 两个 goal 贡献了宏观 wall 差值的 {100 * D_TOP2_SHARE:.1f}%。",
    "",
    "重 goal 的阶段计时直接指向增量匹配：",
    "",
    "| goal | 旧 delta_wall(s) | 当前 delta_wall(s) | 倍数 |",
    "|---|---:|---:|---:|",
    f"| p21 | {D_P21_OLD_DELTA:.3f} | {D_P21_NEW_DELTA:.3f} | {D_P21_NEW_DELTA / D_P21_OLD_DELTA:.2f}x |",
    f"| p34 | {D_P34_OLD_DELTA:.3f} | {D_P34_NEW_DELTA:.3f} | {D_P34_NEW_DELTA / D_P34_OLD_DELTA:.2f}x |",
    "",
    "因此，当前差异主要是重构后动态 G∪Γ 增量匹配及派生路径的实现成本，不是 Deterministic policy 本身突然变化，也不能简单解释为 GPU 比 CPU 慢。历史与新 run 都通过 soundness，但性能结果必须分属各自实现版本。",
    "",
    "## 6. 当前 GPU Deterministic / local Jev 矩阵（216 runs）",
    "",
    "两臂都使用当前 GPU AdaDeduce、deterministic policy 和 exact scorer；Jev 臂只增加初始 star guide。为适配 640 规则和 System One 协议，模型只接收 goal-reachable star，按基础顺序最多取 255 个，并设置 `include_choice_candidates=false`。未送模规则仍完整参与符号执行。",
    "",
    "本地模型为 Qwen3-1.7B，经 LLM2Jev/SGLang 提供 prefill-only Choice 评分。一次 HTTP 请求内部可对最多 255 个候选分别执行 yes/no 评分，不等于一次模型前向。",
    "",
    "| 方法 | hit | soundness | 外部 wall(s) | 内部 wall(s) | 选择分支 | ready 候选 | 完整匹配 | 求值/派生 | guide(s) | cap | timeout |",
    "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
]
for arm, label in (("baseline", "Deterministic"), ("jev", "Deterministic + local Jev")):
    item = NEW["arms"][arm]
    macro = item["macro"]
    lines.append(
        f"| {label} | {item['goal_hit_rate']:.3f} | {item['soundness_rate']:.3f} | "
        f"{macro['wall']:.3f} | {macro['internal_wall']:.3f} | {macro['branches']:,.1f} | "
        f"{macro['ready']:,.1f} | {macro['matches']:,.1f} | {macro['evaluations']:,.1f} | "
        f"{macro['guide_wall']:.3f} | {item['total_caps']} | {item['timeout_runs']} |")
paired = NEW["paired"]
lines += [
    "",
    f"宏平均外部 wall 加速（baseline/Jev）为 **{paired['wall_speedup']:.3f}x**，即 Jev 约慢 {100 * (NEW['arms']['jev']['macro']['wall'] / NEW['arms']['baseline']['macro']['wall'] - 1):.2f}%；求值变化为 **{-paired['evaluation_reduction_pct']:.3f}%**。Jev 中位 wall 在 {paired['jev_wall_wins']}/36 goals 上胜出，在 {paired['jev_evaluation_wins']}/36 goals 上减少求值。",
    "",
    "### 冷启动与重复请求",
    "",
    "| 请求状态 | baseline wall(s) | Jev wall(s) | wall 加速 | 求值减少 | guide(s) |",
    "|---|---:|---:|---:|---:|---:|",
]
for label, key in (("矩阵首轮（主矩阵 p00 已预热）", "seed7_new_goal"),
                   ("seed13/42：相同请求重复", "repeated_requests")):
    item = NEW[key]
    lines.append(
        f"| {label} | {item['baseline']['wall']:.3f} | {item['jev']['wall']:.3f} | "
        f"{item['wall_speedup']:.3f}x | {item['evaluation_reduction_pct']:.3f}% | "
        f"{item['jev']['guide_wall']:.3f} |")
lines += [
    "",
    "重复请求的 guide 成本明显较低，但整体仍没有端到端优势。缓存命中只减少模型计算，不保证模型排序质量提高。",
    "",
    "### 按 Chase round 分层",
    "",
    "| 分层 | goals | baseline wall(s) | Jev wall(s) | wall 加速 | 求值减少 | wall 胜出 | 求值胜出 |",
    "|---|---:|---:|---:|---:|---:|---:|---:|",
]
for stratum in ("round1", "round2", "deep"):
    item = NEW["strata"][stratum]
    lines.append(
        f"| {stratum} | {item['goals']} | {item['baseline_wall']:.3f} | {item['jev_wall']:.3f} | "
        f"{item['wall_speedup']:.3f}x | {item['evaluation_reduction_pct']:.3f}% | "
        f"{item['jev_wall_wins']}/{item['goals']} | {item['jev_evaluation_wins']}/{item['goals']} |")
for suite in SUITES:
    label = suite["label"]
    data = suite["data"]
    lines += [
        "",
        f"### 当前 GPU {label} / {label} + local Jev（216 runs）",
        "",
        "该两臂使用与当前 Deterministic/Jev 矩阵相同的图、规则、goals、seeds、GPU 后端和二进制，只替换底层 action policy；learned 臂使用对应的冻结 value model。",
        "",
        "| 方法 | hit | soundness | 外部 wall(s) | 内部 wall(s) | 求值/派生 | guide(s) | cap | timeout |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for arm, arm_label in (("baseline", label),
                           ("jev", label + " + local Jev")):
        item = data["arms"][arm]
        macro = item["macro"]
        lines.append(
            f"| {arm_label} | {item['goal_hit_rate']:.3f} | {item['soundness_rate']:.3f} | "
            f"{macro['wall']:.3f} | {macro['internal_wall']:.3f} | "
            f"{macro['evaluations']:,.1f} | {macro['guide_wall']:.3f} | "
            f"{item['total_caps']} | {item['timeout_runs']} |")
    suite_paired = data["paired"]
    lines += [
        "",
        f"{label}/Jev 的宏平均外部 wall 加速为 **{suite_paired['wall_speedup']:.3f}x**，求值减少为 **{suite_paired['evaluation_reduction_pct']:.3f}%**；Jev wall 胜出 {suite_paired['jev_wall_wins']}/36 goals，求值减少 {suite_paired['jev_evaluation_wins']}/36 goals。",
    ]
if CPU is not None and CPU["observed_runs"] == 216:
    lines += [
        "",
        "### 修复后 CPU Deterministic / adaptive numeric-Q + local Jev（216 runs）",
        "",
        "该矩阵使用消除逐事实全量索引重建后的 CPU AdaDeduce。baseline 为 Deterministic；Jev 臂为 adaptive numeric-Q，`guide_min_cost=0`、`guide_max_options=64`，因此每个 goal 都启用初始指引。",
        "",
        "| 方法 | hit | soundness | 外部 wall(s) | 内部 wall(s) | 求值/派生 | guide(s) | cap | timeout |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for arm, arm_label in (("baseline", "CPU Deterministic"),
                           ("jev", "CPU adaptive numeric-Q + local Jev")):
        item = CPU["arms"][arm]
        macro = item["macro"]
        lines.append(
            f"| {arm_label} | {item['goal_hit_rate']:.3f} | {item['soundness_rate']:.3f} | "
            f"{macro['wall']:.3f} | {macro['internal_wall']:.3f} | "
            f"{macro['evaluations']:,.1f} | {macro['guide_wall']:.3f} | "
            f"{item['total_caps']} | {item['timeout_runs']} |")
    cpu_paired = CPU["paired"]
    lines += [
        "",
        f"CPU adaptive/Jev 相对当前 CPU Deterministic 的宏平均外部 wall 加速为 **{cpu_paired['wall_speedup']:.3f}x**，求值减少为 **{cpu_paired['evaluation_reduction_pct']:.3f}%**；wall 胜出 {cpu_paired['jev_wall_wins']}/36 goals，求值减少 {cpu_paired['jev_evaluation_wins']}/36 goals。",
        "",
        "| goal | 分层 | CPU D wall | CPU adaptive+Jev wall | wall 加速 | D 求值 | adaptive+Jev 求值 | 求值减少 | guide |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in CPU["paired_rows"]:
        lines.append(
            f"| {row['goal']} | {row['stratum']} | {row['baseline_wall']:.3f} | "
            f"{row['jev_wall']:.3f} | {row['wall_speedup']:.3f}x | "
            f"{row['baseline_evaluations']:,.0f} | {row['jev_evaluations']:,.0f} | "
            f"{row['evaluation_reduction_pct']:.3f}% | {row['guide_wall']:.3f} |")
cold_base = COLD["arms"]["baseline"]["macro"]
cold_jev = COLD["arms"]["jev"]["macro"]
lines += [
    "",
    "### p00 清缓存独立复核",
    "",
    f"清空 SGLang 前缀缓存后，baseline/Jev 求值分别为 {cold_base['evaluations']:,.0f}/{cold_jev['evaluations']:,.0f}，Jev 减少 {COLD['paired']['evaluation_reduction_pct']:.3f}%；但 guide 为 {cold_jev['guide_wall']:.3f}s，外部 wall 从 {cold_base['wall']:.3f}s 增至 {cold_jev['wall']:.3f}s。说明局部搜索收益可以真实存在，但仍可能被模型成本完全抵消。",
    "",
    "## 7. 36 goals 的当前 Jev 逐项对照",
    "",
    "每行先对 seeds `{7,13,42}` 取中位数。wall 单位为秒；求值减少为正表示 Jev 更少。",
    "",
    "| goal | 分层 | baseline wall | Jev wall | wall 加速 | baseline 求值 | Jev 求值 | 求值减少 | guide |",
    "|---|---|---:|---:|---:|---:|---:|---:|---:|",
]
for row in NEW["paired_rows"]:
    lines.append(
        f"| {row['goal']} | {row['stratum']} | {row['baseline_wall']:.3f} | {row['jev_wall']:.3f} | "
        f"{row['wall_speedup']:.3f}x | {row['baseline_evaluations']:,.0f} | {row['jev_evaluations']:,.0f} | "
        f"{row['evaluation_reduction_pct']:.3f}% | {row['guide_wall']:.3f} |")
for suite in SUITES:
    label = suite["label"]
    lines += [
        "",
        f"### {label} + local Jev 的 36-goal 逐项对照",
        "",
        f"| goal | 分层 | {label} wall | +Jev wall | wall 加速 | {label} 求值 | +Jev 求值 | 求值减少 | guide |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for row in suite["data"]["paired_rows"]:
        lines.append(
            f"| {row['goal']} | {row['stratum']} | {row['baseline_wall']:.3f} | "
            f"{row['jev_wall']:.3f} | {row['wall_speedup']:.3f}x | "
            f"{row['baseline_evaluations']:,.0f} | {row['jev_evaluations']:,.0f} | "
            f"{row['evaluation_reduction_pct']:.3f}% | {row['guide_wall']:.3f} |")
lines += [
    "",
    "小 goal 上较大的相对求值减少不一定转化为可见 wall 收益；宏观 wall 又会被 p21、p34 等重 goal 主导，因此报告同时保留逐项数据，不用单一平均数替代完整分布。",
    "",
    "## 8. 历史五方法逐 goal 外部 wall 附录",
    "",
    "下表只用于补充历史统一矩阵的 goal 分布，不与当前 GPU Jev 列混合计算。",
    "",
    "| goal | 分层 | GoalChase | Deterministic | Random | numeric-Q | HDC-Q |",
    "|---|---|---:|---:|---:|---:|---:|",
]
old_maps = {arm: old_goal(arm) for arm in OLD_LABELS}
new_maps = {arm: new_goal(arm) for arm in ("baseline", "jev")}
for goal_id in sorted(new_maps["baseline"]):
    stratum = new_maps["baseline"][goal_id]["stratum"]
    lines.append(
        f"| {goal_id} | {stratum} | " +
        " | ".join(f"{old_maps[arm][goal_id]['wall']:.3f}" for arm in OLD_LABELS) + " |")
lines += [
    "",
    "## 9. 正确性、截断与可重复性",
    "",
    "- 历史统一矩阵：540/540 runs 完成并命中，soundness failures=0，无 timeout；每臂 27 个 run 触发 candidate cap，集中于 p11、p21、p32、p34。",
    "- 当前 Jev 矩阵：216/216 runs 完成并命中，soundness failures=0，无 timeout、无 guide fallback；baseline/Jev 分别有 27/21 个 cap run。",
    "- cap 使命中不能解释为无截断的全局完备性证明；各组状态均为 `complete_with_truncation`。",
    "- 当前 Deterministic/Jev 两臂都只有 2/36 goals 的三次 chain 逐字节一致，当前 GPU 执行存在明显运行间顺序差异。微小求值差异不能全部归因于 policy 或 Jev。",
    "- 历史 CPU 与当前 GPU 的 Deterministic 数字不同，说明实现/后端差异不可忽略；本报告不提供跨两组的全方法墙钟排名。",
]
for suite in SUITES:
    data = suite["data"]
    lines.append(
        f"- 当前 {suite['label']}/Jev 矩阵：{data['observed_runs']}/{data['planned_runs']} runs，"
        f"soundness failures={data['validation_failures']}，"
        f"guide fallbacks={data['arms']['jev']['guide_fallbacks']}。")
lines += [
    "",
    "### 原始产物",
    "",
    "- 历史五方法：`.work_hdcv/fixpoint_unified640/summary.json`、`rows.tsv`、`per_goal.tsv`。",
    "- 当前 Deterministic/Jev：`.work_hdcv/fb15k_jev640/summary.json`、`rows.tsv`、`paired.tsv`。",
]
for suite in SUITES:
    lines.append(
        f"- 当前 {suite['label']}/Jev：`.work_hdcv/fb15k_jev640_{suite['slug']}/summary.json`、`rows.tsv`、`paired.tsv`。")
lines += [
    "- p00 清缓存复核：`.work_hdcv/fb15k_jev640_cold_p00/`。",
    "- 历史结果说明：`experiments/fb15k-237-results/overall-performance.md`。",
    "- Jev 模块与部署：`docs/jev_guide.md`。",
    "",
    "### 聚合与复现入口",
    "",
    "```bash",
    "python3 experiments/fixpoint_policy/run_unified640.py --phase aggregate",
    "CUDA_VISIBLE_DEVICES=2 python3 experiments/llm_guide/run_fb15k_jev.py --engine gpu --phase all",
]
for suite in SUITES:
    slug = suite["slug"]
    if slug == "random":
        policy_args = "--base-policy random"
    else:
        policy_args = ("--base-policy learned --value-model "
                       f".work_hdcv/fixpoint_q640/models/q_{slug}.meta")
        if slug == "hdc":
            policy_args += " --hdc-value-backend original"
    lines += [
        "CUDA_VISIBLE_DEVICES=2 python3 experiments/llm_guide/run_fb15k_jev.py --engine gpu \\",
        f"  --work .work_hdcv/fb15k_jev640_{slug} {policy_args} --phase all",
    ]
lines += [
    "python3 experiments/llm_guide/write_full_report.py",
    "```",
    "",
    "### 本报告读取的汇总文件哈希",
    "",
    f"- `fixpoint_unified640/summary.json`：`{sha256(ROOT / '.work_hdcv/fixpoint_unified640/summary.json')}`",
    f"- `fb15k_jev640/summary.json`：`{sha256(ROOT / '.work_hdcv/fb15k_jev640/summary.json')}`",
]
for suite in SUITES:
    lines.append(
        f"- `fb15k_jev640_{suite['slug']}/summary.json`：`{sha256(suite['path'])}`")
lines += [
    f"- rules640 Chase GT closure：`{sha256(ROOT / '.work_hdcv/fixpoint_rules640/runs/chase/rep1/closure_edges.tsv')}`",
    "",
    "## 10. 最终判断",
    "",
    "在当前证据下，可以支持以下结论：",
    "",
    "1. Goal-directed agent 相比完整 Chase 能避免大量无关计算，但完整 Chase 与逐 goal agent 的工作量不同，必须分开解释。",
    "2. 历史统一矩阵中，Learned numeric-Q 略优于 Deterministic；Random 没有优势，原始 HDC-Q 受模型成本和无效排序共同拖累。",
    "3. 当前本地 Qwen3-1.7B Jev 能在部分 goal 上减少搜索，但没有形成 36-goal 的整体端到端收益，不建议在该设置下默认启用。",
]
for index, suite in enumerate(SUITES, 4):
    data = suite["data"]
    lines.append(
        f"{index}. {suite['label']} + Jev 相对当前 {suite['label']} 的 wall 加速为 "
        f"{data['paired']['wall_speedup']:.3f}x，求值减少 "
        f"{data['paired']['evaluation_reduction_pct']:.3f}%；该组合是否值得启用应按这组直接对照判断。")
if len(SUITES) < 3:
    lines += [
        "",
        "当前 GPU 的完整四 policy 矩阵尚未全部完成；缺失臂不借用历史 CPU 数据冒充。",
        "",
    ]
else:
    lines += [
        "",
        "当前 GPU 的 Deterministic、Random、numeric-Q、HDC-Q ±Jev 已全部按同一协议运行；Chase 和 ChaseDepOnly 按本次范围不重复运行。",
        "",
    ]
OUTPUT.write_text("\n".join(lines))
print(OUTPUT)
