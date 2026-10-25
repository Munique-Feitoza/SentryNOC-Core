#!/bin/sh
set -eu

PREFIX=${SENTRYNOC_PREFIX:-/opt/sentrynoc}
DATA_DIR=${SENTRYNOC_DATA_DIR:-/var/lib/sentrynoc}

if [ "$(id -u)" -ne 0 ]; then
  echo "Execute como root: sudo ./install.sh" >&2
  exit 1
fi

command -v cc >/dev/null || { echo "cc não encontrado"; exit 1; }
command -v python3 >/dev/null || { echo "python3 não encontrado"; exit 1; }

mkdir -p "$PREFIX" "$DATA_DIR"
cc -std=c11 -O2 -Wall -Wextra -Wpedantic src/sentrynoc.c -o "$PREFIX/sentrynoc-agent"
cp -R collector "$PREFIX/"
chmod 0755 "$PREFIX/sentrynoc-agent" "$PREFIX/collector/server.py" "$PREFIX/collector/replay_spool.py" "$PREFIX/collector/forward_spool.py"
cp systemd/*.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now sentrynoc-collector.service
systemctl enable --now sentrynoc-agent.service
systemctl enable --now sentrynoc-forwarder.service
echo "SentryNOC instalado em $PREFIX; dashboard: http://127.0.0.1:8787/"
