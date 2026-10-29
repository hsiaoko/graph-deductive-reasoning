# exp-1 Overall：5 方法 × 50 goal 主对照表

日期：2026-10-29。协议见 NOTES.md（预注册）。goal 构成：ablation 20（18×r1
+ 2×r2）+ 新采样 30（gen-goals seed=77 补齐至 25×r1 + 25×r2），全部
shared-holdout（verify --shared-holdout；bnb 两臂为逐任务留出，协议允许）。

## 主表

| 方法 | 规则集 | 命中 | time-to-goal（主循环） | 派生事实数 | 峰值内存 |
|---|---|---|---|---|---|
| 全量 chase（CPU×8，2 轮，100k 匹配，30M 上限） | rules_g3 81,683 | 48/50 可达（2 个超出截断，见下） | 199.5s 闭包计算（round1 15.0s + round2 184.5s）+ 加载，**一次跑出全部 50 goal** | 23,493,154 | ~50GB（round-2 merge 峰） |
| **backward**（budget=10, cap=8） | rules_g3 | **44/50 全一致 + 2 截断伪影 = 46/50 派生**（r1 25/25，r2 21/25） | mean 1.62s，median 0.25s；r1 mean 0.08s，r2 mean 3.16s | 98（mean 1.96/goal） | ~2-4GB |
| greedy（det+exact，同配置） | rules_g3 | 0/50（地板效应，同消融） | mean 13.7s（全部 budget 耗尽） | — | ~50GB（共享闭包在内） |
| bnb + utility（budget=2, cap=64, top_k=0, 30min 超时） | rules 子集 3,023 | 3/9 可行子集（22 目标子集不可派生记 N/A，19 超时） | 见明细 | 每 goal ≤2 | 10-25GB/worker |
| bnb + hdc-value（同左 + 共享 LOGO 模型） | 同左 | 3/9 同三个 goal | 同上 | 同左 | 同左 |

bnb 两臂在 3 个命中的可行 goal 上展开数完全一致（63/13/33——小树模型无
排序差）；6 个可行但未命中的 goal 两臂都在预算内穷举失败；不可行 goal 上
hdc 臂总展开数 153,441 vs utility 247,986（-38%，但多为无效穷举，不计入
判分）。chase 30M 上限下 bnb 参照行仅用于对照形态，不与 chase 比快慢。

## DIVERGENT 定性（2 个，同谓词 places_lived/location）

**截断伪影，非 agent 不一致。** 证据：①1M 匹配 + 2 轮深挖（逐任务留出）
仍未含目标——但 round-2 merge 单轮即 +28.3M 事实，撞上 30M 派生上限，
闭包根本没走到目标所在轮次；②agent 链逐步骤 dump 在案（div1：8 步、
div2：3 步，每步 EvalAntecedent ok=1，均为合法 chase 步）。改判：两个
goal 计入"派生成功"，soundness 完好（零真实不一致）。文档口径：截断
闭包作 soundness 参照时的局限——超出截断的 agent 派生会误判 DIVERGENT。

## 分层明细（backward，唯一覆盖全 50 的 agent 方法）

| bstar_round | 命中 | 平均耗时 | p90 |
|---|---|---|---|
| round-1 | 25/25 | 80ms | 79ms |
| round-2 | 19/25（+2 截断伪影=21） | 3162ms | 7830ms |

## backward vs chase 加速比

单 goal 延迟：backward mean 1.62s（median 0.25s） vs chase 一次性 227s
（shared closure，含加载）→ **单 goal 快 140×（mean）/900×（median）**；
且 backward 每 goal 只派生 ~2 条事实（chase 派生 23.5M）。口径注意：
chase 一次回答全部 goal（摊销 4.5s/goal），backward 是按需制导——
按需场景胜出三个数量级的派生量与两个数量级的延迟。

## Soundness 与硬控制

- backward：44 CONSISTENT + 2 伪影（已改判）；零真实不一致。
- greedy/bnb 两臂：全程零 DIVERGENT（所有派生 ⊆ 各自闭包）。
- bnb 两臂的 certified_optimal=1 且两臂 chain.tsv 逐字节一致（命中 goal 上）。
- 超时：bnb 两臂各 ~19-21 个 goal 在 30min 上限未决（均为子集下难/不可行
  goal；协议允许记 N/A）。看门狗熔断 3 次（均为深挖 chase 叠加并发所致，
  已处置并改串行/降并发）；当前机器 60GB 已用，无残留进程。

## 复现

```bash
# backward / greedy（rules_g3, shared-holdout）
./build/bin/gar_analyze verify --tasks .work_hdcv/tasks50.tsv \
  --csr <csr> --rules rules_g3 --policy deterministic --scorer exact \
  --executor backward|greedy --max-candidates-per-rule 8 --threads 8 \
  --max-rounds 2 --max-matches 100000 --max-derivations 30000000 \
  --shared-holdout --report out.tsv
# bnb 两臂：.work_hdcv/bnb_arm.sh utility|hdc（逐任务 verify --run, budget 2,
# cap 64, top_k 0, timeout 1800s；hdc 臂加 --bnb-priority hdc-value
# --value-model .work_hdcv/models_shared.meta）
```

## 缺口（如实）

bnb 两臂 19-21/50 goal 在 30min 超时内未决（硬 goal 穷举前沿爆炸），两臂
在这些 goal 上的成绩记 T/O；子集覆盖边界（22/50 目标在 rules 子集不可
派生）是协议内既定 N/A。
