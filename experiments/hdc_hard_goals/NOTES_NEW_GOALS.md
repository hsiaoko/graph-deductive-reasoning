# hdc_hard_goals：新增评估 goal 泛化（预注册补充，2026-11-01）

主 NOTES.md 与判据不变。本补充冻结新增 cohort 与训练隔离，先于运行。

## 新增 cohort（冻结；不随后按输赢淘汰）

候选来自既有成功 trace（**它们曾属旧训练池——如实标注，不称全新
goal**）。按预注册的可观测特征（utility 展开数、best_cost、链长）逐
一核对原始日志后冻结全部 3 个，**含中/低困难对照，不伪造难度**：

| goal | spec | utility 展开 | best_cost | 分层（如实） |
|---|---|---|---|---|
| g9 | /sports/sports_position/players./sports/sports_team_roster/position(/m/02g_6x,/m/02vkdwz) | 265 | 1 | 中（OR 扇出可见） |
| g22 | /tv/tv_program/languages(/m/0584r4,/m/02h40lc) | 63 | 1 | 低-中 |
| g6 | /tv/tv_writer/tv_programs./tv/tv_program_writer_relationship/tv_program(/m/057d89,/m/02_1q9) | 13 | 1 | 易（对照） |

**与锚点的难度差如实声明**：锚点（r2c_2/r2c_4/athletes）为 best_cost=2
且展开 2,084–18,962 的困难证明；新增三个 best_cost=1、展开 13–265，
属中/低难度证明任务——本 cohort 回答的是"value 排序在非困难实例上
是否仍然无害/有益"，不是困难实例泛化。不因此调整 cohort。

## 训练隔离（两臂严格同训练行/标签）

新增 goal 曾属旧训练池，故 HDC 与 numeric **均按新测试 goal 重新训练
LOGO 模型**：成功集 = {g0,g6,g9,g22,r2c_2,r2c_4,r2c_8} − {该 goal}；
失败集 = 同一批失败/熔断 trace（与锚点模型相同）。两模型从同一解析
器输出同一批 (X, y)（标签口径不变：最优路径 y=best−cost，其余 3.0）。
每模型 meta 记录 trained_on/train_nodes（逐折核对）。锚点模型与锚点
结果**不改动**（train_export.py / train_numeric.py 原样保留；新模型
由独立脚本 train_new_goals.py 生成，共享解析/编码口径）。

## 谓词族隔离（判定：做不了，不伪造）

按 consequent 谓词分组：7 个成功 trace 的 head 谓词两两不同
（tv_writer、tv_program/languages 虽同属 /tv/ 大族但谓词不同），每族
仅 1 个 trace → **leave-one-predicate-family-out 无意义样本，明确
N/A**，不伪造分组。

## 运行与判据

同 binary、同图/规则(1652)/budget=2/cap=64/hybrid/exact/top_k=0、串行；
utility / hdc-value / numeric-value + backward 参照（proof_cost 口径
按 2026-11-01 更正）。判据沿用：展开数方向、wall 不劣化、
best_cost/certified/链一致性、soundness。正负结果全部保留并报告。
