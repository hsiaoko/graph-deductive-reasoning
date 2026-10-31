# full_policy_matrix：chase/backward/增量 forward × policy × scorer 全矩阵（预注册）

日期：2026-10-31。用户批准的完整设计，判据写死后不移动。

## Setting（写死）

- 图 `/ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr`；规则
  `/ssd_data/zhuxk/graphs/KG/fb15k-237/rules`（1,652 条）；**no holdout**。
- 5 个 goal（名称沿用，正式 GT 重算）：
  1. `/award/award_nominee/award_nominations./award/award_nomination/award(_,/m/02flpq)`
  2. `/award/award_winner/awards_won./award/award_honor/award_winner(_,/m/02r3zy)`
  3. `/people/person/profession(_,/m/03lgtv)`
  4. `/people/person/places_lived./people/place_lived/location(_,/m/0nvt9)`
  5. `/soccer/football_team/current_roster./soccer/football_roster_position/position(_,_)`
- Agent：budget=1e9；wall-limit=1800s/goal/run（执行器内，干净终止）；
  max-candidates-per-rule=100000；backward --open-collect-cap=10000000；
  forward/backward 均开各自 full-enum。cap 为工程安全参数，触顶明示
  （fwd_capped_enums / chase capped 统计）；termination=timeout 记
  recall@1800s，不冒充最终完备率。
- backward --max-subgoal-depth=64（新增公开开关，默认 4 保持旧命令不
  变；64 > chase 已观测 26 轮；若触及记 depth_limited，不得称无深度
  截断）。本实验只跑 BackwardExecutor（BackwardBnB 只检查透传）。
- 种子解耦（新增 --policy-seed / --hdc-seed，缺省回退 --seed，旧行为
  不变）：HDC encoder 用 hdc-seed；RandomizedPolicy/backward
  random/hybrid/JEV RNG 用 policy-seed。summary 记录两个有效 seed。
  固定 hdc-seed=42；deterministic 各臂 policy-seed=42；random 跑
  policy-seed {7,13,42}；forward hybrid 确定性（admissible+argmax）
  只跑 42；backward hybrid（有洗牌）跑 3 seeds。
- HDC dim=512，其余 scorer 参数默认。
- 矩阵：forward {null,exact,hdc}×{deterministic,random,hybrid}=9 配置
  （random 3 seeds，其余 1）= 15 runs/goal；backward exact×{score,
  random,hybrid}（random/hybrid 各 3 seeds，score 1）= 7 runs/goal；
  chase×1。合计 22 agent runs/goal，110 总目标。forward bnb 不入阵。
- Chase GT：须无有效派生/匹配截断并达 fixpoint（--max-rounds 1000
  --max-derivations 1000000000 --max-matches 1000000000 --print 0）。
  主计时 threads=1（与串行 agent 对齐）；threads=8 只作附加参考。
  chase 一次覆盖 5 goal，报告总时间与 total/5 摊销，不冒充逐 goal
  专属成本。
- 时间主指标 = 执行器 wall（排除共同装载/写盘），端到端附记。
  deterministic 时间重复 3 次取 median（单 run>10min 可一次并说明）；
  random 3 seeds 报 median[min,max]。**所有计时 run 串行，禁并发**。
- 答案：GT = 完整 chase 闭包中 goal 全部匹配。报 all-answer
  recall/precision + derived-only recall（剔基图答案）；chase recall=1
  仅定义。派生 soundness 另行核验（⊆ GT 闭包）。**若 GT 未 fixpoint，
  超出项不判错、不开始正式准确率矩阵**。

## 判据（写死）

1. GT 门槛：chase fixpoint=true 且无 match/derivation 截断，1800s 与
   安全内存内完成；否则停止矩阵、如实报 blocked（不用 capped chase
   当 GT，不缩图/缩规则/缩时间）。
2. 新开关默认回归：不带新 flag 的全部旧命令输出逐字节不变（synth
   bnb/backward 链、fb15k 2 轮 chase 对 .baseline、dyn 增量场景）。
3. 矩阵按 goal 1→5 顺序、每 goal 完成其 22 runs，增量落盘 + 断点续
   跑（已有 summary.yaml 的 run 跳过）；不因 places_lived 困难跳过
   其他 goal。
4. 汇总：每 goal 表 + 总表：wall、all recall、derived-only recall、
   precision、termination、cap/depth 标记、派生数；random 报
   median[min,max]。
5. soundness：每 run 派生 ⊆ GT 闭包；零真实越界。
6. 安全：看门狗 MemAvailable<100GB kill gar_reason/gar_analyze；外部
   timeout 略大于 1800s（用 2000s）；已用内存 >400GB 立即人工 kill；
   重要数据放 .work_hdcv/full_policy_matrix；结束清理看门狗与进程。
7. 时间盒 ~2h：优先 CLI+回归+GT，再逐 goal 矩阵；到点未跑项明确列
   pending + 续跑命令，不宣称全矩阵完成。
