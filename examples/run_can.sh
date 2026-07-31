#!/bin/bash

# Target settings
TARGET_BITRATE="1000000"
TARGET_QLEN="1000"

# List of interfaces to process
INTERFACES=("can0" "can1")

for IFACE in "${INTERFACES[@]}"; do
    IFACE_UPPER=$(echo "$IFACE" | tr '[:lower:]' '[:upper:]')

    # Get current interface status
    CURRENT_BITRATE=$(ip -details link show "$IFACE" 2>/dev/null | grep -oP '(?<=bitrate )\d+')
    CURRENT_QLEN=$(ip link show "$IFACE" 2>/dev/null | grep -oP '(?<=qlen )\d+')
    IS_UP=$(ip link show "$IFACE" 2>/dev/null | grep -c "state UP")

    # Check if bitrate, txqueuelen, and state are all correctly set
    if [ "$CURRENT_BITRATE" = "$TARGET_BITRATE" ] && [ "$CURRENT_QLEN" = "$TARGET_QLEN" ] && [ "$IS_UP" -ge 1 ]; then
        echo "${IFACE_UPPER} is already UP (Bitrate: 1000 Kbps, txqueuelen: ${TARGET_QLEN})."
    else
        echo "Configuring ${IFACE_UPPER} (Bitrate: 1000 Kbps, txqueuelen: ${TARGET_QLEN})..."

        # Bring down the interface first
        sudo /usr/bin/ip link set "$IFACE" down 2>/dev/null

        # Set bitrate and txqueuelen
        sudo /usr/bin/ip link set "$IFACE" type can bitrate $TARGET_BITRATE
        sudo /usr/bin/ip link set "$IFACE" txqueuelen $TARGET_QLEN

        # Bring the interface UP
        sudo /usr/bin/ip link set "$IFACE" up

        echo "${IFACE_UPPER} is now UP at 1000 Kbps with txqueuelen ${TARGET_QLEN}!"
    fi
    echo "----------------------------------------"
done