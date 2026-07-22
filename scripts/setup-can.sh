#!/usr/bin/env bash
set -euo pipefail

interface_name="${1:-can0}"
bitrate="${2:-500000}"
restart_ms="${3:-100}"

if ! [[ "$bitrate" =~ ^[0-9]+$ ]] || ! [[ "$restart_ms" =~ ^[0-9]+$ ]]; then
    echo "Usage: $0 [interface] [bitrate] [restart-ms]" >&2
    exit 2
fi

command -v ip >/dev/null
sudo ip link set "$interface_name" down 2>/dev/null || true
sudo ip link set "$interface_name" type can bitrate "$bitrate" restart-ms "$restart_ms"
sudo ip link set "$interface_name" up
ip -details -statistics link show "$interface_name"
