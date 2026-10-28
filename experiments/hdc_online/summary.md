# hdc_online：HDC 价值函数在线接入 bnb —— A/B 结果

日期：2026-10-28。前轮（hdc_value2/）离线重放 -82.5% 后，用户批准在线
接入。预注册判据见 NOTES.md（①展开数 -20%；②wall 不劣化；③正确性不变）。

## 实现（commit 1f5a0f6）

- `engine/agent/value_model.{h,cpp}`：加载 train_export.py 导出的
  meta+bin（89 个 int8 随机特征向量 + 4097 个 float64 ridge 权重），
  按评测特征预测分支剩余成本；推理计量（calls/ns/maxrss）。
- `--bnb-priority hdc-value --value-model PATH`：evaluate 时 priority =
  cost + 预测剩余成本；fork 子分支继承父分支的模型 key（有前途的父分支
  的孩子先被评估——弥补 deferred-evaluation 的 key=0 FIFO 偏向）。
  **只改排序**：line-4 过滤、剪枝、证书、派生校验一律不动。
- 训练：train_export.py（analyze2.py 同口径特征/分箱/编码），**LOGO 防
  泄漏**：每个测试 goal 的模型训练集明确排除该 goal（构成见下）。
- 默认关闭：athletes 两次复验 chain.tsv 逐字节一致（reg_default、reg2）。

## A/B（rules=3023, budget=2, cap=64, hybrid/exact, top_k=0）

| goal | 展开数 util → hdc | 变化 | wall util → hdc | best_cost | certified | chain.tsv |
|---|---|---|---|---|---|---|
| athletes(g0) | 2084 → 2058 | -1.2% | 200.1s → 191.5s | 2=2 | 1=1 | 一致 |
| r2c_2 | 18962 → 1326 | **-93.0%** | 1781.2s → 124.9s | 2=2 | 1=1 | 一致 |
| r2c_4 | 12791 → 897 | **-93.0%** | 616.3s → 47.9s | 2=2 | 1=1 | 一致 |
| r2c_8 | 68 → 47 | -30.9% | 3.70s → 2.71s | 2=2 | 1=1 | 一致 |
| 合计 | 33905 → 4328 | **-87.2%** | 2711s → 367s | — | — | — |

模型开销：15-21µs/次推理，每 run 总计 0.7-35ms（可忽略）；峰值 RSS
1-12.7GB/进程（串行运行，峰值远低红线）；soundness: pass
（--verify-against-closure）。

## 判据核对

1. 展开数 -20%：**过**（合计 -87.2%；4 goal 中 2 个 -93%，1 个 -31%，
   athletes 持平 -1.2%）。
2. wall 不劣化：**过**（全部 4 goal 均改善，幅度与展开数一致）。
3. 正确性：**过**（best_cost/certified 全同、chain.tsv 全同、soundness
   pass、默认配置逐字节回归×2）。
4. 泄漏控制：每个测试 goal 用 LOGO 模型（训练集 = 其余 6 个成功 goal +
   40 个失败/熔断 goal 的 32.5k 节点，不含该 goal）。

## 差异分析（在线 vs 离线重放）

- r2c_2/r2c_4 在线 -93% 甚至超过离线重放的 -82.5%：模型在这两个 goal 的
  分支特征上有真实区分度，deferred-evaluation 的 key=0 FIFO 波被"继承
  父 key"校正后，好分支的早期扩展立即转化为 Ĉ 收紧和剪枝。
- athletes 在线持平（-1.2%）：该 goal 上模型预测近常数（~2.72，方差
  ~0.02），优先级退化回 cost 序；其离线重放的"win"事后分析部分来自小
  探索树池中的边际排序效应。教训：按 goal 族的特征覆盖决定模型有效性，
  训练集里 athletes 类（大 ncand=250 的 root 分支）样本稀缺。
- deferred-evaluation 口径差：离线重放假设 evaluate-on-push（推送即知
  特征），在线只能在评估后用模型——fork 继承父 key 是本轮补的机制，
  使两口径在好分支族上对齐。

## 最终判定

**在线成功，可进论文主线**（三轮判据全过；两个硬 goal 上展开数与耗时
均降 93%，正确性证据完备）。这是 HDC 系列实验中第一个全通过的部署形态：
HDC 随机特征 ridge 作 bnb best-first 的辅助优先级。后续方向：按 goal 族
补训练样本（athletes 类）、多 seed 模型集成消方差、更大 budget 的可扩展性。
