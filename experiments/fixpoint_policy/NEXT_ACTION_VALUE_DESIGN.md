# 下一步价值标签设计（让 Learned 真减计算量）

2026-11-23。目标：让 learned policy 真正减少推导量（facts），而非仅追平 Deterministic。
本文是设计提案，未实现。原则：Attack the Premise、Exhaust the Design Space、Explain the Number、Laziness Protocol。

## 1. 失败前提（写下来，直接检验）

旧前提：「动作的相关性 = 它在最终证明里离 goal 多近（y=γ^d）。按此排序能减搜索。」

已证伪（测得）：
- FB15k：学会后（MSE 0.064）仍封顶 ≈Det；纯模型派生 +7.3%。
- BioKG：200 goals/hidden4 适定后 MSE 0.2548→0.2226 仍≈均值基线；12 goal 净 ratio 1.316（更差），高方差（p02 灾难 69×）。
- 实例级特征（HDC/图距离）深 goal ≈随机。

前提错在两层：
1. **静态证明属性 ≠ 动态下一步价值**。γ^d 是「事实最终在不在证明上」，不是「现在选它能省多少剩余搜索」。一个证明上的事实可能前提未就绪（现在选不了）；一个证明外的事实可能是必要前置。
2. **标签与 f 同源**。γ^d ≈ f 的 rel_fam=γ^d(谓词)，模型重学 f，蒸馏天花板。

新前提（待检验）：「动作的价值 = 在当前状态下选它后，期望剩余搜索成本的下降。按此排序能减搜索。」

## 2. 限制因子（Explain the Number）

BioKG 搜索被**锥前沿生成**主导（goal 落 round-1 前沿）。派生量 ≈ 前沿大小，与排序弱相关。
但 agent 早停：若策略把 goal 实例排第一，先推它即停，派生骤降。所以**好排序能减派生（facts），但减不了匹配生成（generation）成本**。
用户关心 facts，故价值标签应瞄准「更早命中 goal 实例」。

关键未决经验问题（须原型测定，不能猜）：
- **探索 rollout 里有没有比 Det 更省的轨迹？** 之前硬 goal 探索最好 0.82×Det（弱）。若价值学习要超 Det，训练数据必须含更优轨迹。这是价值路线的生死门。

## 3. 设计空间（Exhaust the Design Space，3 个竞争方案）

### 方案 1：排序损失（同证明信号，换目标函数）— 最便宜，先试
- 监督信号不变（证明成员/距离），但 **MSE 回归 → 决策内 pairwise/listwise 排序损失**。
- 理由：策略用的是排序，直接优化排序比回归绝对 γ^d 更对齐任务、更省样本。MSE 平可能是「预测绝对值难」而非「无信号」。
- 数据：现有 trace 即可，无需新 rollout。
- 检验：recall@1 / pairwise accuracy 是否上升；派生是否减。
- 若排序学会且减派生＝赢；若仍不学＝信号本身不足，转方案 2。

### 方案 2：MC cost-to-go（真·下一步价值，探索 rollout）
- 标签：从决策点 (s,a) 到 goal 的**剩余推导数**（Monte Carlo return），可从轨迹观测。
- 数据：探索性 rollout（random/Gumbel）on 多 train goals，记录 (state, action, remaining-cost)。
- 特征：须加**状态特征**（哪些前提已就绪、已派生集摘要、action 实体到 goal 实体图proximity），现 (v_μ,v_g,f) 缺状态。
- 策略：argmin 预测 cost-to-go。
- 生死门：rollout 须含比 Det 更省的轨迹（先测）。否则 Q^Det 复现 Det，无增益。
- 成本：rollout 贵、噪声大、需状态特征工程。研究性投入。

### 方案 3：goal-动作分类 + 状态特征（简化监督）
- 标签：「该动作是否推出 goal（或直接前置）」的二分类，比回归简单。
- 加状态特征（readiness）。
- 实例级 goal 偏好（f 谓词级分不清 goal 实例，模型用 HDC v_μ·v_g 可分）。
- 介于方案 1/2 之间。

## 4. 推荐序列（Laziness Protocol，最便宜先行）

1. **先方案 1**（排序损失，现有数据）：改损失函数即可，无新数据。测定「MSE 平是目标函数问题还是信号问题」。
2. 若方案 1 学会且减派生 → 采纳，论文 learned 主张立此。
3. 若方案 1 仍不学 → 信号不足，测方案 2 生死门（rollout 有无更优轨迹）。有 → 投方案 2；无 → 价值路线在 BioKG 不可行，learned 贡献回退到策略选择器 C / AdaDeduceStar。

## 5. 经验岔口与判据（写决策再跑）

| 岔口 | 测定 | 改变决策的结果 | 噪声底 |
|---|---|---|---|
| 排序损失能否学会 | 现有数据 pairwise 训练，recall@1 | recall@1 显著 >0 且派生 <Det → 方案 1 赢 | 3 seeds 派生 spread |
| rollout 有无更优轨迹 | random rollout min cost vs Det | min <0.7×Det 且稳定 → 方案 2 有信号 | 多 seed min 分布 |
| 价值信号能否泛化 | 浅 goal 训→深 goal 测 | 深 goal 派生 <Det → 泛化成立 | 跨 stratum |

## 6. 方案 2 完整规格（若生死门通过）

- **数据生成**：每 train goal 跑 K 条 random/Gumbel rollout（K≥8 降方差），记录每决策 (state, action, remaining-derivations)。状态用「已派生事实集 + 就绪前提」摘要。
- **标签**：remaining-cost（回归）或决策内 advantage 排序（更省样本）。优先排序。
- **特征**：v_μ, v_g, f, + 状态特征（readiness 向量、action↔goal 图距离、已派生计数）。
- **模型**：小 hidden（匹配数据量），排序损失。
- **策略**：argmin 预测 cost-to-go，blend f 保底（handcrafted_weight>0）。
- **评估**：held-out goals 派生 vs Det，3 seeds，soundness 对账 GT，完备性（高 cap）。
- **风险**：rollout 贵、深 goal 无更优轨迹（生死门可能否）、状态特征工程量大。

## 7. 诚实边界

即便价值标签成功，BioKG 生成成本（前沿匹配）固定，减的是派生不是匹配。论文须区分「派生量」与「总 wall」。
若生死门否（rollout 无更优轨迹），价值路线在 BioKG 证伪，不夸大；learned 贡献回退 C/AdaDeduceStar（已正增益）。