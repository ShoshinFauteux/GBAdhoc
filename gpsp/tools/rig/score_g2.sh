# score_g2.sh -- score every finished G2 run (WSL, ~/wireless-rig).
cd ~/wireless-rig
for d in g2/*.dir; do
  tag=$(basename "$d" .dir); A=$(cat "$d")
  [ -f "$A/host.log" ] || { echo "== $tag: no logs ($A)"; continue; }
  extra=""
  case $tag in
    emB) extra="--host-code BPEE --join-code BPEE --host-rev 00 --join-rev 00" ;;
    frfr*) extra="--join-code BPRE" ;;
  esac
  turns=$(grep -m1 "^repeat" "$A/host.inputs" | awk '{print $2}')
  echo "== $tag ($A)"
  python3 tools/rig/summarize_battle.py "$A/host.log" "$A/join.log" \
    --host-ini "$A/host.ini" --join-ini "$A/join.ini" \
    --host-fixture "$A/host.inputs" --join-fixture "$A/join.inputs" \
    --turns "${turns:-30}" $extra --json "$A/score.json" 2>&1 \
    | grep -E "VERDICT|INVALID|DESYNC  |turns_completed|f2m_L|f2m_R|ap_fail|fail_probe" | cut -c1-260
done
