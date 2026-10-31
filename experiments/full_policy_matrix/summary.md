# full_policy_matrix：BLOCKED at GT gate（如实报告）

日期：2026-10-31。预注册：NOTES.md（判据未移动）。CLI commit ace3ce2。

## 结论：正式矩阵未启动

按预注册判据 1，chase GT 门槛（无截断真实 fixpoint，1800s + 安全内存）
**未通过**，正式 110-run 矩阵未启动。未使用任何 capped chase 充当 GT，
未缩图/缩规则/缩时间。

## 门槛实测证据

配置：`--max-rounds 1000 --max-derivations 1e9 --max-matches 1e9
--no-matches --print 0`，fb15k-237 全图 × 1,652 规则。

| 尝试 | 结果 |
|---|---|
| threads=1 | round 1-4 完成（+205,701 / +442,894 / +3,905,330 /
  **+58,996,674**；round 4 enumerate 310s + merge 104s）；**round 5
  uncapped 枚举 ~1500s 未完成**，外部 timeout 2000s 杀死。RSS 峰值观测
  ~89GB 且仍在爬升。总派生已 ≥64M 且远未收敛 |
| threads=8 | round 4 enumerate 45.7s（6.8× 加速）+ merge 118.2s，
  派生数逐数相同（uncapped 内容确定性 ✓）；**round 5 枚举约 9 分钟时
  RSS 达 311.8GB、以 ~25GB/min 增长、MemAvailable 124GB**——按用户红
  线（已用>400GB 立即 kill）与看门狗（<100GB）双重协议人工提前终止 |

参照：同一图/规则在 1M match-cap 下 26 轮 29.67M 派生仍
`fixpoint: false`（round≥5 被 cap 截断）——即**该规则集在此图上的
真实闭包显著大于 30M 且需要 >5 轮**，uncapped round 5 的中间
embedding 空间超出 1800s/400GB 资源包络。

## 已完成部分（commits）

1. `0c2d916` 预注册 NOTES.md（setting + 判据，未移动）。
2. `ace3ce2` CLI 公平性开关：
   - `--max-subgoal-depth N`（默认 4 不变；BackwardExecutor 与
     BackwardBnBExecutor 均已透传 ExecConfig.max_subgoal_depth）
   - `--policy-seed` / `--hdc-seed`（缺省回退 --seed，旧行为逐字节不
     变）；HDC encoder 用 hdc-seed，RandomizedPolicy/backward 洗牌
     RNG/JEV mock 用 policy-seed；summary.yaml 记录
     seed/policy_seed/hdc_seed/max_subgoal_depth。
   - 默认回归：synth bnb/backward 链逐字节一致、fb15k 2 轮 chase 与
     .baseline 逐字节一致、dyn 增量场景 4 步命中、--policy-seed 7 下
     HDC 数值不变。
3. docs/commands.md 新开关文档（同 commit 后补，见 git log）。
4. 断点续跑矩阵脚本 + 聚合脚本（本目录 run_matrix.sh / aggregate.py；
   110 run 目标、goal 1→5 顺序、det 3 次计时取 median、random 3 seeds
   median[min,max]、增量落盘跳过已完成 run）。

## 续跑条件（解除 blocked 的可选路径，供用户决策）

以下任何一条都需要用户明确批准（超出本次 setting）：
- 更大的资源包络（round 5 需要多少内存/时间未知，可能数倍于观测值）；
- 对 chase 引擎做内存改造（流式枚举/外存 merge）——属引擎级改动；
- 改 setting（缩规则/缩图/capped GT）——预注册判据明确禁止，需重新
  批准。

矩阵脚本就绪，一旦 GT 可用：先 `python3 aggregate.py` 依赖的
`gt_closure/closure_edges.tsv` 落位，再 `bash run_matrix.sh`（串行、
断点续跑）。

## 环境

看门狗与全部 gar 进程已清理；阻塞观测点内存已恢复（421GB 可用）。
