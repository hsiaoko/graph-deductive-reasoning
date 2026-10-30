# full_matrix：大答案集完整 baseline 矩阵（budget=1e9 + 30min 墙钟）

日期：2026-10-30。预注册：FULL_MATRIX_NOTES.md。最小扩展 commit
30aa28b（--forward-full-enum + --wall-limit，默认逐字节回归）。

**Setting**：fb15k-237 全基图 × 1,652 规则子集；4 个大答案集 goal；
GT = 基图 ∪ capped chase（10M/4 轮）；全部臂 budget=1e9、执行器内
wall-limit 1800s（干净终止、部分结果照常落盘）、match cap 100000
（backward 加 collect cap 1e7）。chase 基线：89.3s 完备（10M 闭包）。

**臂**：bwd_score / bwd_rand42 / bwd_hybrid / bwd_hdc(512) /
fwd_exact / fwd_hdc(512)；bnb 两臂 N/A（见文末说明）。

## 汇总表（方法为行）

（跑完填入：平均 recall / 平均 wall / 终止形态）

## 逐 goal recall

（跑完填入）

## 结论

（待填：forward greedy 全枚举表现 = 核心新数据；HDC 两臂是否无增益）

## bnb 臂为何 N/A

forward bnb（utility / hdc-value）是**单链最优证书**语义：它在预算内
找一条 cost 最小的 goal 证明链并给最优性证书（Theorem 5），回答的
是"存不存在一条便宜证明"，不是"答案集的全外延"。用 bnb 逐答案跑
1e9 预算只是在重复证明已知名单，不产生外延语义，故标 N/A 不跑。

## soundness 与回归

- --forward-full-enum / --wall-limit 默认关闭，synth bnb/backward
  链逐字节回归通过（commit 30aa28b）。
- 全部派生经 EvalAntecedent 门控（合法 chase 步）；beyond-10M-GT
  部分以 30M 高帽闭包抽检（open_full 已建）。

## 复现

```bash
bash .work_hdcv/fm_arm.sh           # 24 runs: 4 goals x 6 arms
python3 .work_hdcv/fm_aggregate.py  # → .work_hdcv/fm_detail.tsv
```
