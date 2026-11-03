/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Xanadu Quantum Technologies Inc. */
/*
 * mrmac-up - bring the VPK120 MRMAC 100G link up from user space.
 *
 * The ERNIC build did this inside the patched xilinx_axienet driver on `ifconfig eth0 up`.
 * HSB owns the MAC's AXIS now and there is no netdev, so this replays that driver's
 * sequence (the ERNIC build's kernel patches in ../ernic_hwhs/code/meta-user/recipes-kernel/linux:
 * fix-mrmac-issues-vpk120, add_rx_datapath_ctl, align_delays, vpk120_mrmac_tweaks) through the
 * same registers:
 *
 *   MAC   reset, 100G mode, RS-FEC (528,514), CONFIG_RX/TX, release
 *   GT    reset all lanes (gtctrl GPIO), wait for reset-done, RX-datapath reset
 *   link  clear latched status, wait for RX alignment (one RX-datapath reset retry),
 *         then PCS block lock
 *
 * Pause frames are not configured: HSB does not use flow control.
 *
 *   mrmac-up [-s] [-t timeout_s]      -s: only print the link status
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "hwhs.h"

#include <getopt.h>
#include <time.h>
#include <unistd.h>

/* MRMAC registers */
#define MRMAC_RESET 0x0004u
#define MRMAC_MODE 0x0008u
#define MRMAC_CONFIG_TX 0x000Cu
#define MRMAC_CONFIG_RX 0x0010u
#define MRMAC_FEC_CONFIG1 0x00D0u
#define MRMAC_TX_STS 0x0740u
#define MRMAC_RX_STS 0x0744u
#define MRMAC_STATRX_BLKLCK 0x0754u

#define MRMAC_STS_ALL 0xFFFFFFFFu
#define MRMAC_CTL_DATA_RATE_MASK 0x00000007u /* GENMASK(2,0) */
#define MRMAC_CTL_AXIS_CFG_MASK 0x00000E00u  /* GENMASK(11,9) */
#define MRMAC_CTL_SERDES_W_MASK 0x00000070u  /* GENMASK(6,4) */
#define MRMAC_CTL_DATA_RATE_100G 4u
#define MRMAC_CTL_AXIS_CFG_100G (5u << 9) /* 100G, independent 384-bit */
#define MRMAC_CTL_SERDES_W_100G (6u << 4)
#define MRMAC_CTL_PM_TICK (1u << 30)
#define MRMAC_RSFEC ((1u << 12) | 0x8u) /* TX four-lane PMD | RS-FEC op mode */
#define MRMAC_CONFIG_RX_VAL 0x00000403u
#define MRMAC_CONFIG_TX_VAL 0x01000C07u
#define MRMAC_RX_ALIGN_MASK ((1u << 16) | (1u << 2) | (1u << 1) | (1u << 0))
#define MRMAC_RX_100G_BLKLCK_MASK 0x000FFFFFu

/* GT GPIO values (cips_block xlslice map: gtctrl [7:0] line rate, [11:8] reset all,
 * [30:24] main cursor; gt_debug [7:4] rx datapath reset, [13:8] pre, [19:14] post cursor) */
#define GT_CTRL_RESET 0x4B000F02u
#define GT_CTRL_RUN 0x4B000002u
#define GT_RX_DATAPATH_RESET 0x000243F0u
#define GT_RX_DATAPATH_RUN 0x00024300u
#define GT_LOCK_MASK 0xFu

static volatile uint32_t *mac, *gt_lock, *gt_ctrl, *gt_rx;

static uint32_t rd(volatile uint32_t *b, uint32_t off) { return b[off / 4]; }
static void wr(volatile uint32_t *b, uint32_t off, uint32_t v) { b[off / 4] = v; }

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int wait_gt_lock(double timeout_s)
{
    double t0 = now_s();
    uint32_t v;
    while (((v = rd(gt_lock, GPIO_DATA)) & GT_LOCK_MASK) != GT_LOCK_MASK) {
        if (now_s() - t0 > timeout_s) {
            fprintf(stderr, "mrmac-up: GT reset done 0x%x, expected 0xf\n", v & GT_LOCK_MASK);
            return (v & GT_LOCK_MASK) ? 0 : -1; /* the driver accepted any lane */
        }
        usleep(20);
    }
    return 0;
}

static void mac_reset(void)
{
    usleep(10000);
    wr(mac, MRMAC_RESET, MRMAC_STS_ALL);
    usleep(1000);

    uint32_t mode = rd(mac, MRMAC_MODE);
    mode &= ~(MRMAC_CTL_DATA_RATE_MASK | MRMAC_CTL_AXIS_CFG_MASK | MRMAC_CTL_SERDES_W_MASK);
    mode |= MRMAC_CTL_DATA_RATE_100G | MRMAC_CTL_AXIS_CFG_100G | MRMAC_CTL_SERDES_W_100G;
    wr(mac, MRMAC_FEC_CONFIG1, MRMAC_RSFEC);
    mode |= MRMAC_CTL_PM_TICK;
    wr(mac, MRMAC_MODE, mode);
    wr(mac, MRMAC_CONFIG_RX, MRMAC_CONFIG_RX_VAL);
    wr(mac, MRMAC_CONFIG_TX, MRMAC_CONFIG_TX_VAL);
    wr(mac, MRMAC_RESET, 0);
}

static int gt_rx_reset(void)
{
    usleep(10000);
    wr(gt_rx, GPIO_DATA, GT_RX_DATAPATH_RESET);
    usleep(10000);
    wr(gt_rx, GPIO_DATA, GT_RX_DATAPATH_RUN);
    usleep(20000);
    return wait_gt_lock(2.0);
}

static int gt_reset(void)
{
    usleep(10000);
    wr(gt_ctrl, GPIO_DATA, GT_CTRL_RESET);
    wr(gt_ctrl, GPIO_DATA, GT_CTRL_RUN);
    if (wait_gt_lock(2.0)) {
        return -1;
    }
    return gt_rx_reset();
}

static void print_status(void)
{
    uint32_t rx = rd(mac, MRMAC_RX_STS), tx = rd(mac, MRMAC_TX_STS),
             bl = rd(mac, MRMAC_STATRX_BLKLCK);
    printf("MRMAC: RX_STS 0x%08x (status %u aligned %u) BLKLCK 0x%05x TX_STS 0x%08x GT reset done "
           "0x%x\n",
           rx, rx & 1, (rx >> 1) & 1, bl & MRMAC_RX_100G_BLKLCK_MASK, tx,
           rd(gt_lock, GPIO_DATA) & GT_LOCK_MASK);
}

static int wait_link(double timeout_s)
{
    usleep(1000);
    wr(mac, MRMAC_TX_STS, MRMAC_STS_ALL);
    wr(mac, MRMAC_RX_STS, MRMAC_STS_ALL);
    wr(mac, MRMAC_STATRX_BLKLCK, MRMAC_STS_ALL);

    double t0 = now_s();
    int first = 1;
    while (!(rd(mac, MRMAC_RX_STS) & MRMAC_RX_ALIGN_MASK)) {
        wr(mac, MRMAC_RX_STS, MRMAC_STS_ALL);
        sleep(1);
        if (first) { /* alignment is flaky on the first try */
            gt_rx_reset();
            first = 0;
        }
        sleep(1);
        if (now_s() - t0 > timeout_s) {
            fprintf(stderr, "mrmac-up: RX alignment timeout\n");
            return -1;
        }
    }
    while (!(rd(mac, MRMAC_STATRX_BLKLCK) & MRMAC_RX_100G_BLKLCK_MASK)) {
        wr(mac, MRMAC_STATRX_BLKLCK, MRMAC_STS_ALL);
        sleep(2);
        if (now_s() - t0 > timeout_s) {
            fprintf(stderr, "mrmac-up: RX block lock timeout\n");
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    int status_only = 0, c;
    double timeout_s = 16.0; /* XCMAC_RX_ALIGN_TIME_OUT */

    while ((c = getopt(argc, argv, "st:")) != -1) {
        switch (c) {
        case 's':
            status_only = 1;
            break;
        case 't':
            timeout_s = atof(optarg);
            break;
        default:
            fprintf(stderr, "usage: mrmac-up [-s] [-t timeout_s]\n");
            return 2;
        }
    }

    mac = hwhs_map(HWHS_PHYS_MRMAC, HWHS_SIZE_MRMAC);
    gt_lock = hwhs_map(HWHS_PHYS_GT_RST_MASK, HWHS_SIZE_GPIO);
    gt_ctrl = hwhs_map(HWHS_PHYS_GT_CTRL, HWHS_SIZE_GPIO);
    gt_rx = hwhs_map(HWHS_PHYS_GT_DEBUG, HWHS_SIZE_GPIO);
    if (!mac || !gt_lock || !gt_ctrl || !gt_rx) {
        return 1;
    }

    if (!status_only) {
        mac_reset();
        if (gt_reset()) {
            print_status();
            return 1;
        }
        int rc = wait_link(timeout_s);
        print_status();
        if (rc) {
            return 1;
        }
        uint32_t rx = rd(mac, MRMAC_RX_STS);
        if ((rx & 0x3) != 0x3) {
            fprintf(stderr, "mrmac-up: warning: RX status/aligned bits are not both set\n");
        }
        printf("mrmac-up: link up\n");
        return 0;
    }
    /* the status bits latch faults (status latches low), e.g. from link-up at boot:
     * clear them so this shows the link now */
    wr(mac, MRMAC_TX_STS, MRMAC_STS_ALL);
    wr(mac, MRMAC_RX_STS, MRMAC_STS_ALL);
    wr(mac, MRMAC_STATRX_BLKLCK, MRMAC_STS_ALL);
    usleep(1000);
    print_status();
    return 0;
}
