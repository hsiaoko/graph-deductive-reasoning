# HDC 公平实验 NOTES

日期：2026-10-27。任务：给 HDC 公平场景 + 优化实现 + 判定"HDC 有没有用"。

## 既有不公平点（代码确认）

HdcScorer::Score 每候选：① 每前提 EncodeFact 两次（v_mu 打包 + 命中探测各一）；
② 每次余弦都重算 EvidenceVector::Packed()（O(D) + 每个零坐标一次 splitmix）；
③ 符号向量每次 FromHash 现生成（D/64 次 splitmix/符号）。证据束 E 在同一批
候选间不变，这些全是重复劳动。

## 优化（只提速，不改打分数值）

- WitnessLog::PackedHdc()：E 的打包向量按 witness 事件失效缓存（drity flag）。
- HdcScorer 事实向量 memo（相等比较的 full key，哈希冲突不会串值；1M 封顶清空）。
- 每前提只编码一次，v_mu 与命中探测共用。
- EvidenceVector::Packed 的零坐标 tie 位按维度进程级缓存（同 splitmix 流，逐位相同）。
- v_mu 打包仍走 int16 累加器原路径（语义保持）。

## 语义不变验证

- synth hdc（dim 8192, greedy, lives_in(p0,t1)）：chain.tsv 与既有运行
  （/tmp/p5a/hdc）逐字节一致；summary 共同字段一致（新增字段为后续无关提交引入）。
- 旧代码（git worktree HEAD=/tmp/garagent_old）vs 新代码 A/B：
  synth lives_in(p0,t1)、works_at(p1,c0) chain.tsv 逐字节一致；
  fb15k olympics 探针（dim 512, budget 6）chain.tsv 逐字节一致
  （old 1.14s → new 0.66s 端到端，加载占大头）。

## 微基准结果（独立 hdc 场景，合成事实，P=4 前提/候选，30% 前提命中见证集）

D=8192，µs/候选（微基准二进制的库函数随构建演进，分两轮记录）：

| bundle m | 候选数 | exact | hdc 旧实现 | hdc 优化(算法) | hdc 优化(算法+native) |
|---|---|---|---|---|---|
| 8 | 1k/10k/100k | 0.021/0.094/0.119 | ~485 | ~220 | ~74-80 |
| 32 | 同上 | 0.021/0.116/0.112 | ~458 | ~221 | 待补 |
| 128 | 同上 | 0.020/0.105/0.110 | ~437 | ~222 | 待补 |
| 512 | 同上 | 0.030/0.136/0.136 | ~426 | ~223 | 待补 |
| 2048 | 同上 | 0.034/0.144/0.148 | ~424 | ~224 | 待补 |
| 8192 | 同上 | 0.036/0.151/0.158 | ~422 | ~224 | 待补 |

要点：hdc 旧实现成本被"每次余弦重打包 E"主导（与 m 无关 ≈420-480µs）；
优化后 ≈220µs（v_mu 的 O(P·D) int16 累加 + Packed 是语义底线），native
编译再降至 ~78µs。但 exact 是 0.02-0.16µs —— **每候选打分 HDC 差 3 个数量级**，
结构上追不平（O(P·D) vs O(P) 哈希查询）。

## 近似质量（D=8192, tau=0.15, 上表同场景）

- rel_evd 排序 Kendall tau：m=8-32 → 1.00；m=128 → 0.96；m=512 → 0.74；
  m=2048 → 0.47；m=8192 → 0.26-0.30。top-1 一致率 m≤128 全对，m≥512 崩。
- 逐前提命中判定（cos≥tau）：m=8 全对；m≥32 跌到 0.24-0.31——信号幅度
  ~sqrt(2/(πm)) 在 m≳44 时跌破固定 tau=0.15（App I 噪声尺度的实测印证）。
- 结论：HDC 打分在小证据束（m≲100）是 exact 的好近似；大束退化。exact 无此问题。

## Phase 4 原型：子目标经验泛化（离线，closure_edges.tsv 23.5M 行）

设计：8 个高产关系 × 各 1200 正例（闭包内）+1200 负例（同 src 池随机 dst），
19200 个子目标，agent 同款编码（pred=splitmix(FNV(name))|1<<63，顶点=id，
arity 2），4M 随机对测余弦→结局一致率。

结果（D=8192，基线一致率 0.500）：
- [0,0.05)：2.02M 对，一致 0.500（=基线）；
- [0.15,0.30)（同谓词+共享一端能达到的最高带，coscheck 实测 cos≈0.25）：
  1395 对一致 0.540；其中同 src 534 对一致 0.509 ≈ 基线；
- [0.3,0.7)：0 对（不存在）；[0.7,1]：199 对一致 1.000——全是恒等/重复对。
- 对照：cos(同 pred+src 不同 dst)=0.250，cos(仅同 pred)≈0，cos(无关)≈0，
  cos(恒等)=1。

判定：**无信号**。当前编码让不同子目标近正交（这正是打分角色需要的性质），
唯一高余弦对是恒等对；同谓词同源点的子目标结局一致率与基线无差异。
"子目标经验泛化"在现有编码下不可行；要做需要换成保留结构相似性的编码
（新功能而非优化，超出本期范围）。

## 微基准最终表（native 库，µs/候选）

D=8192：exact 0.07-0.16；hdc 旧 240-280；hdc 优化 73-80（加速比 3.0-3.8×）。
D=512：exact 同上；hdc 旧 ~12.4；hdc 优化 4.8-6.5（2.5×）。
两维度下近似质量规律一致：rel_evd 排序 tau 在 (m+N)² ≤ D（App I 可靠界）内
≈1.0，越界后单调退化（8192: m=512→0.74, m=8192→0.26；512: m=32 即 0.75+）。
hit_agree（逐前提 cos≥0.15）：m≥32 即崩（信号 ~1/√m 跌破固定 tau）。

## 系统级（fb15k rules_g3, 20 任务, shared-holdout, 每步 ~65 万候选）

- backward + hdc512：**20/20 CONSISTENT**（与 exact 相同，命中不回归；
  平均 agent_wall_ms 见 tsv）——backward 的尝试排序不依赖 rel_evd。
- greedy：exact 14.13s/任务（主循环均值）；hdc512 旧实现 43.6s（历史值）；
  hdc512 优化（仅算法，pre-native 二进制）31.5s；hdc512 优化+native **21.06s**
  （=exact 的 1.49×；纯打分开销 30.1s→6.9s，降 4.4×）。

## 2026-10-27 补测：bit-packed + SIMD（用户追问"SIMD 和 bit 计算也没用吗"）

- 机器：Xeon Gold 5320（Ice Lake），AVX2/AVX512F/BW/VL，无 VPOPCNTDQ
  （向量 popcount 走 512-bit shuffle 查表）。
- 新工具：bench_bitpacked.cpp（速度）、bench_bitquality.cpp（质量），均在
  experiments/hdc_fair/ 可复现。
- 结果详见 bitsimd_summary.md。一句话：D=512/P≥8 时 bit+AVX512 比 exact
  快 1.6-2.4×，但该维度的 App I 可靠界 m≤22 远低于真实见证束（数十~数百），
  质量不可用；质量安全的 D=8192 下 bit 路径输 4-8×。二值化相对 int16 不丢
  排序质量（tie 差异仅 0.91-1.00 逐位一致）。结论维持：不进主线。
