#!/usr/bin/env bash
set -euo pipefail

BASE="/home/pi/projects/laboratoire3"
OUT="$BASE/PROFILAGE_REMISE"
RUN_SECS=22
PLOT_SECS=15

mkdir -p "$OUT"

for N in $(seq -w 1 11); do
  SCRIPT=$(ls "$BASE/configs/${N}"_*.bash | head -n 1)

  echo "=============================="
  echo "Scenario $N -> $SCRIPT"
  echo "=============================="

  sudo rm -f "$BASE"/profilage-*.txt "$BASE"/stats.txt 2>/dev/null || true

  bash "$SCRIPT" &
  SCENPID=$!

  sleep "$RUN_SECS"

  sudo pkill -INT compositeur 2>/dev/null || true
  sleep 1
  sudo killall sudo 2>/dev/null || true
  kill "$SCENPID" 2>/dev/null || true

  mkdir -p "$OUT/$N"
  mv "$BASE"/profilage-*.txt "$OUT/$N/" 2>/dev/null || true
  mv "$BASE"/stats.txt "$OUT/$N/" 2>/dev/null || true

  python3 "$BASE/creerProfilageImages.py" "$OUT/$N" \
    --sortie "$OUT/$N/profilage_${N}.png" \
    --duree "$PLOT_SECS" 2>/dev/null || true

  echo "OK -> $OUT/$N"
done

echo "DONE -> $OUT"