#!/usr/bin/env bash
set -euo pipefail

interface_name=${1:-vcan0}

command -v ip >/dev/null
sudo modprobe vcan

if ! ip link show $interface_name >/dev/null 2>&1; then
    sudo ip link add dev $interface_name type vcan
fi

sudo ip link set $interface_name up
ip -details link show $interface_name
