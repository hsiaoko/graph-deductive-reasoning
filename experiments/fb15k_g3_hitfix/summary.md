# fb15k-237 / rules_g3：agent 命中率修复（hitfix）总结

日期：2026-10-26。前置实验：experiments/fb15k_g3_agent_vs_chase（对比实验）与
experiments/fb15k_g3_ablation（消融，命中率 0/20）。本目录是"发现原因→修改→
验证"的最终记录。

## 根因（一句话）

**goal motif 从未进入候选列表**：agent 的候选生成是前向全图枚举 + 每规则
cap=8 截断，而能一步推出 goal 的 embedding 在枚举序的第 288~16086 位
（logs/rootcause_*.txt：两个任务各 34/34、61/61 条可一步派生的规则全部被
截断，0 条在 cap 内）——打分/decoy 分析是次要的，正确的 motif 根本进不了
候选集。

证据链：① chase 截断闭包逐任务给出派生规则与轮次；② /tmp/root_cause 工具
直接定位 goal embedding 的枚举位置；③ greedy 链显示 conf=1 的高分规则反复
中奖但与 goal 无关（decoy 形态 = 同 SCC 的高置信规则）。

## 修复（commit ecaf034 及当日后续）

1. `graph::PatternMatcher::Enumerate` 新增 pins 重载：`pins[v]=w` 强制
   emb[v]==w，种子扫描在 pin 位于种子锚点时用 slot 有序 pair 数组二分切片
   （pin 在 dst 端时走翻转槽位），候选绑定处校验 driver 边存在性。空 pins 时
   枚举序与旧版逐字节一致（回归验证）。
2. 新 `BackwardExecutor`（`--executor backward`，engine/agent）：
   - head 谓词索引：只对 consequent == goal 谓词的规则做 pinned 枚举；
   - 打分排序（scorer 插件保留：rel_fam/rel_evd/conf），确定性序
     （total 降序，规则号/embedding 平局）；
   - 前提缺边 → 子目标递归：X 边谓词失败（dynamic）递归 derive；pattern 边
     缺失但可派生 → 开放该边（孤立 pin 顶点从子模式剔除并重映射 id）、递归
     派生开放边、再全模式 pinned 重匹配；两条开放边共享一个自由变量时先从
     一侧 derive_collect 收集候选值再逐一校验另一侧；
   - 每次 EvalAntecedent 计 1 单位（论文口径），预算共享，active-set 防环，
     max_subgoal_depth=4。
   - 语义红线保持：每次 ApplyDerivation 都是合法 chase 步（EvalAntecedent +
     当前图），派生 ⊆ 闭包。

## 迭代史

- 起点（消融实验）：0/20（所有 greedy/random 配置）。
- 第一轮（backward executor 初版）：18/20，全部 CONSISTENT，0 DIVERGENT。
- 第三轮（双开协调 + 排序制导 + 局部预算 + 单边规则子目标化）：18/20 保持
  （无回归），两个 round-2 任务仍未中——机制障碍与下一步见 NOTES.md 第三轮。

## 最终对照表（20 任务 workload，verify --tasks --shared-holdout）

| 口径 | 命中 | 说明 |
|---|---|---|
| agent greedy（上轮全部配置） | 0/20 | 见 fb15k_g3_ablation |
| **agent backward（本期）** | **18/20** | 全部 CONSISTENT，0 DIVERGENT；每任务 72-185ms |
| 全量 chase（2 轮 100k 截断） | 20/20（可达性 ground truth） | 227s 一次（共享） |

未命中 2 个 = 仅有的两个 bstar_round=2 任务（film/actor 与 award_nomination/
award）。根因：它们的推导需要两条各自由规则派生的中间边共享自由变量
（gar_004814 需 award_nominee(015rkw,x2) ∧ nominated_for(x2,0fg04)，x2 要同时
满足两个派生）——当前实现的自由变量协调只覆盖单缺边主干。budget 30 也不中
（30 步全是有效但不对口的派生）。未来工作：join-coordinated subgoal
derivation（两侧各派生候选集再求交）。

## 耗时对比（命中任务）

- backward agent：每任务 72-185ms（含 pinned 枚举）；固定开销（规则加载+DG）约
  39s 每次进程。
- 全量 chase：227s（2 轮截断）+ 48GB 内存。

即：对单个 goal，agent 比全量 chase 快约 3 个数量级（主循环），且只派生
目标链上的事实（每任务 1-2 条 vs chase 的 2350 万条）。

## 回归

synth：full 模式对 .baseline 逐字节一致；greedy（5a 用例）与 bnb（5b 用例）
chain.tsv 逐字节不变；verify --tasks 默认路径（逐任务留出）12/12 不变。
PatternMatcher pins 重载空 pins 路径与旧实现逐字节一致。

## 复现

```bash
./build/bin/gar_analyze verify --tasks <tasks.tsv> --csr <csr> --rules rules_g3 \
  --policy deterministic --scorer exact --executor backward \
  --max-candidates-per-rule 8 --threads 8 --max-rounds 2 --max-matches 100000 \
  --max-derivations 30000000 --shared-holdout --report out.tsv
```
