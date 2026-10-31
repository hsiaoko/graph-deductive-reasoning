# full_policy_matrix @ rules256（中间档；最新为 rules512）

> **规模说明（2026-10-31 更新）**：本文件是 **N=256 中间档**。最新规
> 模为 **N=512**（`summary512.md`，g5 部分 pending 待续跑；前缀嵌套
> rules256 ⊂ rules512 已程序验证）。

日期：2026-10-31。预注册：NOTES.md + RULE_SUBSET.md + RULE_SCALEUP.md
（判据未移动）。**本文件是当前最终规模（N=256）的结果；summary.md 是
N=64 中间档；更早的 1,652 规则实验（blocked）不与本轮混比。**

## Setting 执行核对

- fb15k-237 全图 × rules256（确定性续选，前缀嵌套 rules64 ⊂ rules128
  ⊂ rules256 程序验证通过：manifest256.tsv 前 64 行与 rules64 逐条一
  致）；5 goal 不变；**未降级到 128**。
- 矩阵参数不变：budget=1e9、wall=1800s、cap=100000、open-collect-cap
  1e7、depth=64（无 run 触及）、HDC512、hdc-seed=42、random seeds
  {7,13,42}、det/hyb 计时 3 次 median（全部 <10min，3 次都跑了）。
- **110/110 matrix runs + 70 计时重复全部完成**，串行，无 timeout run。
- soundness：110 run 全部派生 ⊆ GT256 fixpoint 闭包，**零越界**；各臂
  各 goal answer-precision 全 1.000。

## GT @ rules256（无 cap 真实 fixpoint）

| | rules256 | rules64（对照） |
|---|---|---|
| 轮数 / 派生 | **18 轮 / 222,146，fixpoint=true，零 cap** | 8 轮 / 25,515 |
| wall threads=1（主） | **3.58s** | 0.46s |
| wall threads=8（参考） | 1.59s（闭包逐字节一致） | 0.38s |
| 峰值 RSS | 207MB | 183MB |
| 摊销口径 | 总 3.58s / 5 ≈ 0.72s/goal（非逐 goal 专属成本） | 同口径 |

GT 答案（base+derived / max_round / degenerate）：

| goal | rules256 | rules64 |
|---|---|---|
| g1 award_nominations | 5 (4+1) r1 | 5 (4+1) 不变 |
| g2 awards_won | 6 (0+6) r1 | 6 不变 |
| g3 profession | **955 (8+947) r16，不再 degenerate** | 8 (8+0) degenerate |
| g4 places_lived | 0，仍 degenerate | 0 |
| g5 position | **5,527 (2273+3254) r6** | 2,737 (2273+464) |

## 总表（recall / derived-only recall / 执行器 wall；det·hyb=3 次 median，random=3 seeds median[min,max]）

| 方法 | g2 wall | g3 | g5 rec / drec / wall |
|---|---|---|---|
| chase（摊销） | 0.72s/goal | 1.0 | 1.0 / 1.0 |
| bwd score | 4.817s [4.815,4.822] | 1.0/1.0 @0.104s | **0.974 / 0.955 / 0.392s** |
| bwd random×3 | 4.8-6.7s | 同 | 0.974 / 0.955（全 seed 同） |
| bwd hyb×3 | 4.8s | 同 | 同 |
| fwd null det | 9.874s [9.669,9.906] | 1.0/1.0 @0.050s | 1.0 / 1.0 / 0.745s |
| fwd exact det | 37.821s [37.688,38.170] | 1.0/1.0 @0.113s | 1.0 / 1.0 / 0.985s |
| fwd hdc det | **225.475s** [225.152,227.787] | 1.0/1.0 @0.633s | 1.0 / 1.0 / 5.693s |
| fwd exact random×3 | 50.2-51.4s | 同 recall | 1.0，**7.7-7.9s** |
| fwd hdc random×3 | 299.8-303.9s | 同 | 1.0，42.8-44.7s |

g1：22 臂全 1.000 @ ~1ms。g4：空 GT，全臂 n/a（recall 不定义）、
precision 1.000、毫秒级、0 派生。

## 相对 rules64 的变化（规模对照，非加速比）

1. **GT 规模**：8 轮/25,515 → 18 轮/222,146（8.7×）；g3 从 degenerate
   变为 955 答案（20 条 profession head 规则 + 祖先入子集）；g4 结构性
   空集不变（4 条 head 规则在 256 内仍推不出 /m/0nvt9 的答案）。
2. **recall**：forward 在两个规模的全部非退化 goal 上均为 100%/100%；
   backward g5 all-recall 0.991→**0.974**，derived-only 0.946→**0.955**，
   两个指标方向不同，但绝对漏答从 25 增至 **145 条派生答案**。因此稳定
   的结论是 backward g5 的结构性覆盖缺口随 GT 增大而放大，而不是“所有
   recall 都不变”或“derived-only 也下降”。
3. **wall 趋势**（同 goal 不同 N，仅趋势）：g2 bwd 26ms→4.8s（185×，
   DG 锥变大需探更多死路）；g2 fwd exact 2.6s→37.8s；HDC 溢价随规模
   放大（g2：20.3s→226s；null:exact:hdc = 1:3.8:22.8 @256 vs
   1:3.4:26 @64）。
4. **policy 不改变最终 recall，但搜索成本差异在 256 档放大**：forward
   random 比 det 慢约 1.3×（g2 exact：50.2s vs 37.8s）至 8×（g5 exact：
   7.8s vs 0.99s；hdc rand 44s vs 5.7s），recall 逐数一致。backward
   recall 也无分化，但 g2 random 的一个 seed 达 6.67s、高于 det 4.82s，
   不能说 backward 时间无分化。该结论延续并放大 rules64 的观察。
5. **HDC 仍只有开销**：所有 hdc 臂 recall 与 exact 逐数一致。确定性臂
   HDC 相对 exact 约 **5.6–6.0×**；相对 null 的倍数随 goal 为
   **7.6–22.8×**（g2/g3/g5），两种分母不混用。
6. **方向边界**：小答案集 g2 上 backward 目标制导更快（4.8s/6 派生 vs
   forward 9.9s+/33,868 派生）；g3 上 forward 与 backward 都完备，且
   forward+null 0.050s 快于 backward 约 0.10s；g5 上 forward 完备，
   backward 有 145 条派生答案缺口。不能概括成 backward 在所有大答案集
   都不完备，缺口是逐 goal 的结构性现象。
7. **chase 口径**：一次 3.58s 覆盖 5 goal，摊销约 0.72s/goal。g3 的
   forward/backward 均低于该摊销；g2 所有 agent 臂都高于摊销；g5 的
   forward+null 0.745s 与摊销相近但略慢，backward 更快却不完备。多 goal
   或需要完整闭包时 chase 仍有明确价值。
8. g1 仍只有 1 个派生答案、g4 为空 GT；主要非平凡证据来自 g2/g3/g5，
   不把 256 条规则的结果外推到不可行的 1,652 条完整矩阵。

## 复现 / 续跑

```bash
python3 experiments/full_policy_matrix/select_rules.py \
  /ssd_data/zhuxk/graphs/KG/fb15k-237/rules .work_hdcv/full_policy_matrix 256
./build/bin/gar_reason --mode full --engine cpu \
  --csr /ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr \
  --rules .work_hdcv/full_policy_matrix/rules256 \
  --out .work_hdcv/full_policy_matrix/gt_closure256 --threads 1 --print 0 \
  --max-rounds 1000 --max-derivations 1000000000 --max-matches 1000000000 --no-matches
bash experiments/full_policy_matrix/run_matrix256.sh   # 串行断点续跑
python3 experiments/full_policy_matrix/aggregate256.py
```

产物：`.work_hdcv/full_policy_matrix/{rules256,gt_closure256,runs256,
manifest256.tsv,gt_answers256.tsv,detail256.tsv}`；仓库内：
manifest256.tsv、detail256.tsv、select_rules.py（N 参数化）、
run_matrix256.sh、aggregate256.py。无 pending。
