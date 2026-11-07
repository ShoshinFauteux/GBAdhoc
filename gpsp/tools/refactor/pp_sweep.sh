#!/usr/bin/env bash
# pp_sweep.sh -- the variant -E sweep of PLAN-P-beta5 §4 (common): G0 (pp_diff.sh) on
# the arms no built flavour compiles, one arm at a time, -E only, no link.
#
#   pp_sweep.sh --ref TREE_A --new TREE_B --logs RUN_DIR [--overlay DIR] [--keep DIR]
#               [--arms "name name ..."] [--list]
#
# Each arm is the -D/-U a tools/build.sh switch puts on main_psp.o's compile line,
# appended to the flavour's own line (pp_diff.sh --extra-defs), so the arm is
# preprocessed exactly as build.sh would build it.  Arms whose switch the tree's
# tools/build.sh does not accept are PRUNED (listed, not run): build.sh is the one
# place the supported configurations live.
#
#   arm          flavour line     extra defs                 build.sh switch
#   mgift0       release          -UGBADHOC_MGIFT            MGIFT=0 (psp/Makefile)
#   rewind0      release          -UREWIND                   REWIND=0
#   link0        release          -UGBA_LINK                 GBA_LINK=0
#   online0      release          -UGBADHOC_ONLINE           ONLINE=0
#   rfu0         release          -URFU_SCHED                RFU_SCHED=0 (the stable line, on the beta line)
#   rfu1         release-stable   -DRFU_SCHED                RFU_SCHED=1 (on the stable line)
#   cheats0      release          -UGPSP_CHEATS              CHEATS=0
#   lowlat0      release          -DPLAY_ME_LOWLAT=0         ME_LOWLAT=0
#   lowlat1      release          -DPLAY_ME_LOWLAT=1         ME_LOWLAT=1
#   fresh0       release          -DGPSP_NO_PLAY_INPUT_FRESH INPUT_FRESH=0 (build.sh: not -DPLAY_INPUT_FRESH=0)
#
# Exit 0 when every arm that runs is IDENTICAL, 1 a difference, 2 an arm that does
# not preprocess (or usage).  The summary line lists the arms run and pruned.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ARMS_ALL="mgift0 rewind0 link0 online0 rfu0 rfu1 cheats0 lowlat0 lowlat1 fresh0"
REF= NEW= LOGS= OVL= KEEP= ARMS= LIST=0
while [ $# -gt 0 ]; do
  case "$1" in
    --ref) REF=$2; shift 2 ;; --new) NEW=$2; shift 2 ;; --logs) LOGS=$2; shift 2 ;;
    --overlay) OVL=$2; shift 2 ;; --keep) KEEP=$2; shift 2 ;; --arms) ARMS=$2; shift 2 ;;
    --list) LIST=1; shift ;;
    -h|--help) sed -n '2,27p' "$0"; exit 0 ;;
    *) echo "pp_sweep: unknown arg $1" >&2; exit 2 ;;
  esac
done
arm() {   # arm NAME -> "flavour|defs|build.sh switch text (fixed string)"
  case "$1" in
    mgift0)  echo 'release|-UGBADHOC_MGIFT|MGIFT="${MGIFT' ;;
    rewind0) echo 'release|-UREWIND|REWIND="${REWIND' ;;
    link0)   echo 'release|-UGBA_LINK|GBA_LINK="${GBA_LINK' ;;
    online0) echo 'release|-UGBADHOC_ONLINE|ONLINE="${ONLINE' ;;
    rfu0)    echo 'release|-URFU_SCHED|RFU_SCHED="${RFU_SCHED' ;;
    rfu1)    echo 'release-stable|-DRFU_SCHED|RFU_SCHED="${RFU_SCHED' ;;
    cheats0) echo 'release|-UGPSP_CHEATS|CHEATS="${CHEATS' ;;
    lowlat0) echo 'release|-DPLAY_ME_LOWLAT=0|-DPLAY_ME_LOWLAT=' ;;
    lowlat1) echo 'release|-DPLAY_ME_LOWLAT=1|-DPLAY_ME_LOWLAT=' ;;
    fresh0)  echo 'release|-DGPSP_NO_PLAY_INPUT_FRESH|-DGPSP_NO_PLAY_INPUT_FRESH' ;;
    *) return 1 ;;
  esac
}
[ -n "$ARMS" ] || ARMS=$ARMS_ALL
for a in $ARMS; do arm "$a" >/dev/null || { echo "pp_sweep: unknown arm $a" >&2; exit 2; }; done
if [ "$LIST" = 1 ]; then for a in $ARMS; do echo "$a $(arm "$a")"; done; exit 0; fi
[ -n "$REF" ] && [ -n "$NEW" ] && [ -n "$LOGS" ] || { echo "pp_sweep: --ref, --new and --logs are required" >&2; exit 2; }
topath() { case "$1" in [A-Za-z]:[/\\]*) wslpath -a "$1" ;; *) (cd "$1" 2>/dev/null && pwd) || echo "$1" ;; esac; }
BS="$(topath "$NEW")/tools/build.sh"
[ -f "$BS" ] || { echo "pp_sweep: no $BS" >&2; exit 2; }
RC=0; RUN=(); PRUNED=(); BAD=()
for a in $ARMS; do
  IFS='|' read -r fl defs sw <<< "$(arm "$a")"
  if ! grep -qF -- "$sw" "$BS"; then PRUNED+=("$a(build.sh has no $sw)"); continue; fi
  if [ ! -f "$(topath "$LOGS")/$fl/logs/.build-eboot.log" ]; then PRUNED+=("$a(no $fl log)"); continue; fi
  echo "== arm $a: $fl line $defs"
  bash "$HERE/pp_diff.sh" --ref "$REF" --new "$NEW" --logs "$LOGS" --flavours "$fl" --extra-defs "$defs" \
    ${OVL:+--overlay "$OVL"} ${KEEP:+--keep "$KEEP/$a"} 2>&1 | sed 's/^/   /'
  r=${PIPESTATUS[0]}
  echo "   -> arm $a rc $r"
  RUN+=("$a:$r")
  [ "$r" = 0 ] || { BAD+=("$a"); [ "$r" = 2 ] && RC=2; [ "$RC" = 0 ] && RC=1; }
done
echo "pp_sweep: ran ${RUN[*]:-none}; pruned ${PRUNED[*]:-none}"
echo "G0var $([ $RC = 0 ] && echo "ok×${#RUN[@]}" || echo "FAIL ${BAD[*]}")"
exit $RC
