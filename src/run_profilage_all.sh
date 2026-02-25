#!/usr/bin/env bash
set -euo pipefail

BASE="/home/pi/projects/laboratoire3"
OUT="$BASE/PROFILAGE_REMISE"

RUN_SECS=25        # temps de run par scenario
COOLDOWN_SECS=120  # pause entre scénarios

stop_all() {
  # 1) essaie arrêt doux
  sudo pkill -INT compositeur 2>/dev/null || true
  sleep 1

  # 2) TERM sur programmes + wrappers sudo
  sudo pkill -TERM -f "./(decodeur|compositeur|filtreur|redimensionneur|convertisseur)" 2>/dev/null || true
  sudo pkill -TERM decodeur compositeur filtreur redimensionneur convertisseur 2>/dev/null || true
  sudo pkill -TERM -f "sudo ./((decodeur|compositeur|filtreur|redimensionneur|convertisseur))" 2>/dev/null || true
  sleep 1

  # 3) KILL si reste encore
  sudo pkill -KILL -f "./(decodeur|compositeur|filtreur|redimensionneur|convertisseur)" 2>/dev/null || true
  sudo pkill -KILL decodeur compositeur filtreur redimensionneur convertisseur 2>/dev/null || true
  sudo pkill -KILL -f "sudo ./((decodeur|compositeur|filtreur|redimensionneur|convertisseur))" 2>/dev/null || true
  sleep 1
}

mkdir -p "$OUT"

for N in $(seq -w 1 11); do
  SCRIPT=$(ls "$BASE/configs/${N}"_*.bash | head -n 1)

  echo "=============================="
  echo "Scenario $N -> $SCRIPT"
  echo "=============================="

  # Stop + clean AVANT le run
  stop_all
  sudo rm -f /dev/shm/mem* "$BASE"/profilage-* "$BASE"/stats.txt 2>/dev/null || true

  # Run scenario en background
  bash "$SCRIPT" &
  SCENPID=$!

  # Laisser tourner
  sleep "$RUN_SECS"

  # Stop + kill du script de scénario
  stop_all
  kill "$SCENPID" 2>/dev/null || true

  # Ranger résultats
  mkdir -p "$OUT/$N"
  mv -f "$BASE"/stats.txt "$OUT/$N/" 2>/dev/null || true
  mv -f "$BASE"/profilage-* "$OUT/$N/" 2>/dev/null || true

  echo "Résultats -> $OUT/$N"
  ls -lh "$OUT/$N" | head -n 20 || true

  # Vérification qu'il ne reste rien qui tourne
  if pgrep -af "decodeur|compositeur|filtreur|redimensionneur|convertisseur" >/dev/null 2>&1; then
    echo "ATTENTION: il reste des processus actifs. Liste:"
    pgrep -af "decodeur|compositeur|filtreur|redimensionneur|convertisseur" || true
    echo "On force un dernier stop_all..."
    stop_all
  fi

  # Cooldown + check thermique
  echo "Cooldown ${COOLDOWN_SECS}s..."
  sleep "$COOLDOWN_SECS"
  (vcgencmd measure_temp && vcgencmd get_throttled) 2>/dev/null || true
  echo
done

echo "DONE. Tous les résultats sont dans $OUT"