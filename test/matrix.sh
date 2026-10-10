#!/bin/sh
# opera-boot acceptance matrix (GPL-2.0-only).
#
# Every run gates on harness exit 0, a kernel-handoff marker, no resident
# fatal trap, and decoded visible content at one of four checkpoints.
# Optional out-of-tree panafz1 goldens add boot-signature and CDCMD checks.
#
# Usage: test/matrix.sh <biosfile> [frames]
set -u

BIOS=${1:-build/opera-boot.bin}
case $BIOS in
  /*) ;;
  *)  BIOS=`cd "$(dirname "$BIOS")" && pwd`/`basename "$BIOS"` ;;
esac
[ -f "$BIOS" ] || { echo "matrix.sh: no such bios: $BIOS" >&2; exit 2; }

FRAMES=${2:-2400}
if [ "$FRAMES" -lt 2400 ]; then
  echo "matrix.sh: FRAMES=$FRAMES < 2400 makes the screenshot gate meaningless" >&2
  exit 2
fi

DEV=${DEV:-`cd "$(dirname "$0")/../.." && pwd`}
OPERA=${OPERA:-$DEV/opera-libretro}
GOLD=${GOLD:-`cd "$(dirname "$0")" && pwd`/golden}
RUNS=${RUNS:-`cd "$(dirname "$0")" && pwd`/runs}
CD_SPEED=${CD_SPEED:-2x}
DISCS=${DISCS:-}
mkdir -p "$RUNS"

run_one() {
  name=$1; iso=$2
  out=$RUNS/$name
  mkdir -p "$out"
  (cd "$OPERA" && OPERA_CDROM_CMDTRACE=1 timeout 300 ./opera-test-harness \
     --core ./opera_libretro.so --bios "$BIOS" --title "$iso" \
     --frames "$FRAMES" \
     --screenshot-at 900="$out/f900.png" --screenshot-at 1200="$out/f1200.png" \
     --screenshot-at 1800="$out/f1800.png" --screenshot-at 2400="$out/f2400.png" \
     --option opera_kprint=enabled --option opera_random_seed=1 \
     --option opera_cd_speed="$CD_SPEED" \
     --output-dir "$out" --wall-timeout 120) >/dev/null 2>&1
  rc=$?

  pass=1; why=""
  [ $rc -eq 0 ] || { pass=0; why="exit=$rc"; }
  if ! grep -qE 'boot [0-9A-Fa-f]{8}' "$out/run.log" 2>/dev/null; then
    pass=0; why="$why boot-marker"
  fi
  if grep -qE '(^|[[:space:]])TRAP (undf|pabt|dabt|addr) lr=[0-9A-Fa-f]{8}' \
    "$out/run.log" 2>/dev/null; then
    pass=0; why="$why trap"
  fi

  if [ -f "$GOLD/$name/boot-signature.txt" ]; then
    sed -E 's/\(\$[0-9A-Fa-f]+\)/($ADDR)/' "$GOLD/$name/boot-signature.txt" > "$out/.sig"
    sed -E 's/\(\$[0-9A-Fa-f]+\)/($ADDR)/' "$out/run.log" > "$out/.run"
    while IFS= read -r line; do
      [ -z "$line" ] && continue
      grep -qF -- "$line" "$out/.run" || {
        pass=0
        why="$why missing-sig:$(echo "$line" | cut -c1-40)"
      }
    done < "$out/.sig"
  fi

  # CDCMD comparison is meaningful only for the byte-identical dump used
  # to capture the optional golden.  Report by default; STRICT=1 gates it.
  if [ -f "$GOLD/$name/run.log" ]; then
    if ! python3 test/cmp_reads.py \
      "$GOLD/$name/run.log" "$out/run.log" "$iso" 2> "$out/.cdcmp"; then
      if [ "${STRICT:-0}" = "1" ]; then
        pass=0; why="$why cdcmd:$(head -c 80 "$out/.cdcmp")"
      else
        why="$why cdcmd(warn)"
      fi
    fi
  fi

  anyblack=1
  for f in "$out/f900.png" "$out/f1200.png" "$out/f1800.png" "$out/f2400.png"; do
    if [ ! -s "$f" ]; then pass=0; why="$why shot-missing"; continue; fi
    if python3 test/png_visibility.py "$f"; then anyblack=0; fi
  done
  [ $anyblack -eq 0 ] || { pass=0; why="$why shot-black"; }

  if [ $pass -eq 1 ]; then status="PASS"; else status="FAIL($why)"; fi
  printf '%-14s %s\n' "$name" "$status"
  return $((1-pass))
}

ret=0
ran=0
run_if_present() {
  name=$1; iso=$2
  if [ ! -f "$iso" ]; then
    printf '%-14s SKIP (disc absent)\n' "$name"
    return
  fi
  ran=$((ran + 1))
  run_one "$name" "$iso" || ret=1
}

if [ -n "$DISCS" ]; then
  # Read newline-delimited name=path entries without shell word splitting.
  while IFS= read -r entry; do
    [ -z "$entry" ] && continue
    run_if_present "${entry%%=*}" "${entry#*=}"
  done <<EOF
$DISCS
EOF
else
  run_if_present return-fire   "$DEV/3do-return-fire-decomp/oracle.iso"
  run_if_present total-eclipse "$DEV/3do-total-eclipse-decomp/oracle.iso"
  run_if_present poed          "$DEV/3do-poed-decomp/oracle.iso"
  run_if_present shadow        "$DEV/3do-shadow-decomp/oracle.iso"
  run_if_present helloworld    "$DEV/3do-devkit/iso/helloworld.iso"
  run_if_present terminus      "$DEV/opera-libretro/terminus.iso"
  if [ -n "${CNB_ISO:-}" ]; then
    run_if_present cnb "$CNB_ISO"
  fi
fi

if [ $ran -eq 0 ]; then
  echo "matrix.sh: no disc fixtures found" >&2
  exit 2
fi
exit $ret
