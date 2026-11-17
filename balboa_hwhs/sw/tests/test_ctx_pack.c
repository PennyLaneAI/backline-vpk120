/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Xanadu Quantum Technologies Inc. */
/*
 * test_ctx_pack.c - the QP and connection contexts the driver writes into QP_CTX / CONN_CTX
 * (hwhs_qp_ctx_words, hwhs_conn_ctx_words), word for word against values worked out from the
 * layouts in hwhs.h. hw/hls/csim feeds the same words to BALBOA's own C++; this test needs
 * nothing but a C compiler.
 */
#include <stdio.h>

#include "hwhs.h"

int main(void)
{
    static const uint32_t qp_ref[6] = {0x00000000u, 0x56000000u, 0x34561234u,
                                       0x34abcd12u, 0x34000012u, 0x007f0012u};
    static const uint32_t conn_ref[6] = {0x0abc0000u, 0x00a8c000u, 0x00a8c001u,
                                         0x00a8c001u, 0x00a8c001u, 0x00c00001u};
    struct hwhs_qp qp = {
        .local_qpn = 0,
        .remote_qpn = 0x000ABC,
        .psn = 0x123456,
        .rkey = 0x1234ABCD,
        .ring_iova = 0x7F0012340000ull,
        .remote_ip = 0xC0A80001u,   /* 192.168.0.1 */
        .udp_sport = 0xC000,
    };
    uint32_t q[6], c[6];
    int bad = 0;

    hwhs_qp_ctx_words(&qp, q);
    hwhs_conn_ctx_words(&qp, c);
    for (int i = 0; i < 6; i++) {
        if (q[i] != qp_ref[i]) {
            printf("QP_CTX[%d] = 0x%08x, expected 0x%08x\n", i, q[i], qp_ref[i]);
            bad++;
        }
        if (c[i] != conn_ref[i]) {
            printf("CONN_CTX[%d] = 0x%08x, expected 0x%08x\n", i, c[i], conn_ref[i]);
            bad++;
        }
    }
    printf("%s\n", bad ? "FAIL" : "context words OK");
    return bad ? 1 : 0;
}
