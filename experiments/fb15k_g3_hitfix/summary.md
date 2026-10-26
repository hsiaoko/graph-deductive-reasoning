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
- 第四轮（双侧派生协调，commit d380d16）：**20/20 全部 CONSISTENT**。
  候选集并入基图现成值（此前只看"可派生"）、修逆边 pin 侧 bug、
  剩余子模式边做半连接过滤求交（中间边缺而一级可派生时才计费派生）、
  尝试按候选证据分层排序、子模式回退的计费递归先过免费预检。
  详见 NOTES.md 第四轮。

## 最终对照表（20 任务 workload，verify --tasks --shared-holdout）

| 口径 | 命中 | 说明 |
|---|---|---|
| agent greedy（上轮全部配置） | 0/20 | 见 fb15k_g3_ablation |
| agent backward（第三轮） | 18/20 | 全部 CONSISTENT，0 DIVERGENT |
| **agent backward（本期，双侧派生协调）** | **20/20** | 全部 CONSISTENT；budget=10；两 round-2 任务 spent=6 与 2 |
| 全量 chase（2 轮 100k 截断） | 20/20（可达性 ground truth） | 227s 一次（共享） |

两个 round-2 任务（film/actor 与 award_nomination/award）在第四轮命中：
机制 = 两条开放边各自取候选（基图现成值优先、一级可派生其次），剩余
子模式边做半连接过滤求交（中间边缺失但一级可派生时计费派生），组合对
再全模式校验派生。budget 敏感性：两任务在 budget=3/5/10 下均命中。

## 耗时对比（命中任务）

- backward agent：每任务 72-185ms（含 pinned 枚举）；固定开销（规则加载+DG）约
  39s 每次进程。
- 全量 chase：227s（2 轮截断）+ 48GB 内存。

即：对单个 goal，agent 比全量 chase 快约 3 个数量级（主循环），且只派生
目标链上的事实（每任务 1-2 条 vs chase 的 2350 万条）。

## 回归

synth：full 模式对 .baseline 逐字节一致；greedy（5a 用例）与 bnb（5b 用例）
chain.tsv 逐字节不变；verify --tasks 默认路径（逐任务留出）12/12 不变（bnb 与
backward 均 12/12 CONSISTENT）。PatternMatcher pins 重载空 pins 路径与旧实现
逐字节一致。第四轮改动后全部复验通过。

## 复现

```bash
./build/bin/gar_analyze verify --tasks <tasks.tsv> --csr <csr> --rules rules_g3 \
  --policy deterministic --scorer exact --executor backward \
  --max-candidates-per-rule 8 --threads 8 --max-rounds 2 --max-matches 100000 \
  --max-derivations 30000000 --shared-holdout --report out.tsv
```
