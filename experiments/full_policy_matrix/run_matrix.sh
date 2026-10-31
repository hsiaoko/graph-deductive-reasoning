#!/bin/bash
# full_policy_matrix runner: serial, resumable (skips runs with summary.yaml),
# goal order 1->5, 22 agent runs per goal (+ timing repeats for det-family).
cd /home/zhuxk/project/graph/GARAgent
W=.work_hdcv/full_policy_matrix
OUT=$W/runs
mkdir -p $OUT
CSR=/ssd_data/zhuxk/graphs/KG/fb15k-237/immutable_csr
RULES=/ssd_data/zhuxk/graphs/KG/fb15k-237/rules
COMMON="--budget 1000000000 --wall-limit 1800 --max-candidates-per-rule 100000 --hdc-seed 42"
FWD="--executor greedy --forward-full-enum"
BWD="--executor backward --backward-full-enum --open-collect-cap 10000000 --max-subgoal-depth 64"

run_one() {  # $1=dir $2=goal $3...=args
  local d=$1 spec=$2; shift 2
  if [ -f "$d/summary.yaml" ]; then return; fi
  mkdir -p $d
  timeout 2000 ./build/bin/gar_reason --mode agent --csr $CSR --rules $RULES \
    --goal "$spec" --out $d "$@" > $d/run.log 2>&1
  echo "$(basename $(dirname $d))/$(basename $d) rc=$?"
}

goal_spec() {  # 1..5
  case $1 in
    1) echo '/award/award_nominee/award_nominations./award/award_nomination/award(_,/m/02flpq)';;
    2) echo '/award/award_winner/awards_won./award/award_honor/award_winner(_,/m/02r3zy)';;
    3) echo '/people/person/profession(_,/m/03lgtv)';;
    4) echo '/people/person/places_lived./people/place_lived/location(_,/m/0nvt9)';;
    5) echo '/soccer/football_team/current_roster./soccer/football_roster_position/position(_,_)';;
  esac
}

for gi in 1 2 3 4 5; do
  spec=$(goal_spec $gi)
  gd=$OUT/g$gi
  # ---- forward: scorer x policy; random 3 seeds, det/hybrid seed 42 ----
  for scorer in null exact hdc; do
    local_hdc=""; [ "$scorer" = "hdc" ] && local_hdc="--hdc-dim 512"
    # deterministic (x3 timing reps; skip reps if rep1 > 600s)
    d0=$gd/fwd_${scorer}_det
    run_one $d0/rep1 "$spec" $COMMON $FWD --scorer $scorer $local_hdc --policy deterministic --policy-seed 42
    w1=$(grep -oE 'wall=[0-9.e]+' $d0/rep1/run.log 2>/dev/null | tail -1 | grep -oE '[0-9.e]+')
    big=$(awk -v w="$w1" 'BEGIN{print (w+0>600)?1:0}')
    if [ "$big" = "0" ]; then
      run_one $d0/rep2 "$spec" $COMMON $FWD --scorer $scorer $local_hdc --policy deterministic --policy-seed 42
      run_one $d0/rep3 "$spec" $COMMON $FWD --scorer $scorer $local_hdc --policy deterministic --policy-seed 42
    else
      echo "fwd_${scorer}_det g$gi: single-rep (wall>$600s)" >> $W/timing_notes.txt
    fi
    # random x 3 policy seeds
    for ps in 7 13 42; do
      run_one $gd/fwd_${scorer}_rand_s$ps "$spec" $COMMON $FWD --scorer $scorer $local_hdc --policy random --policy-seed $ps
    done
    # hybrid (deterministic at rule+ motif argmax level) x3 timing reps
    d0=$gd/fwd_${scorer}_hyb
    run_one $d0/rep1 "$spec" $COMMON $FWD --scorer $scorer $local_hdc --policy hybrid --policy-seed 42
    w1=$(grep -oE 'wall=[0-9.e]+' $d0/rep1/run.log 2>/dev/null | tail -1 | grep -oE '[0-9.e]+')
    big=$(awk -v w="$w1" 'BEGIN{print (w+0>600)?1:0}')
    if [ "$big" = "0" ]; then
      run_one $d0/rep2 "$spec" $COMMON $FWD --scorer $scorer $local_hdc --policy hybrid --policy-seed 42
      run_one $d0/rep3 "$spec" $COMMON $FWD --scorer $scorer $local_hdc --policy hybrid --policy-seed 42
    else
      echo "fwd_${scorer}_hyb g$gi: single-rep (wall>$600s)" >> $W/timing_notes.txt
    fi
  done
  # ---- backward: exact scorer; order score x1(+reps), random x3, hybrid x3 ----
  d0=$gd/bwd_score
  run_one $d0/rep1 "$spec" $COMMON $BWD --scorer exact --backward-order score --policy-seed 42
  w1=$(grep -oE 'wall=[0-9.e]+' $d0/rep1/run.log 2>/dev/null | tail -1 | grep -oE '[0-9.e]+')
  big=$(awk -v w="$w1" 'BEGIN{print (w+0>600)?1:0}')
  if [ "$big" = "0" ]; then
    run_one $d0/rep2 "$spec" $COMMON $BWD --scorer exact --backward-order score --policy-seed 42
    run_one $d0/rep3 "$spec" $COMMON $BWD --scorer exact --backward-order score --policy-seed 42
  else
    echo "bwd_score g$gi: single-rep (wall>$600s)" >> $W/timing_notes.txt
  fi
  for ps in 7 13 42; do
    run_one $gd/bwd_rand_s$ps "$spec" $COMMON $BWD --scorer exact --backward-order random --policy-seed $ps
    run_one $gd/bwd_hyb_s$ps "$spec" $COMMON $BWD --scorer exact --backward-order hybrid --policy-seed $ps
  done
  echo "GOAL-$gi-DONE"
done
echo MATRIX-DONE
