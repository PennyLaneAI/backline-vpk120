#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.
#
# VPK120 boot setup for backline-roce. No ERNIC modules: BALBOA is in the PL, hwhs and
# BALBOA are driven from user space through UIO.

# Management network (the TCP handshake with the coprocessor host)
ip addr add 192.168.3.15/24 dev end0
ip link set end0 up

# Bind the PL windows marked "generic-uio" in system-user.dtsi to uio_pdrv_genirq.
# (No bootargs needed: driver_override binds them by device.)
for dev in /sys/bus/platform/devices/*; do
    [ -e "$dev/driver" ] && continue
    grep -q generic-uio "$dev/of_node/compatible" 2>/dev/null || continue
    echo uio_pdrv_genirq > "$dev/driver_override"
    echo "$(basename "$dev")" > /sys/bus/platform/drivers/uio_pdrv_genirq/bind
done

# 100G link for BALBOA (replaces the axienet MRMAC bring-up)
/usr/bin/mrmac-up || echo "vpk-init: mrmac-up failed; check the cable / far end and run mrmac-up again"

/usr/bin/balboactl status || true
