/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Xanadu Quantum Technologies Inc. */
/*
 * test_ecb_frame - check that hwhs.c builds the same ECB frame as hw/sim/tb_hwhs_hsb.sv
 * (RD_DWORD 0x80, seq 1). Run with make test.
 */
#include "../lib/hwhs/hwhs.c"

static const uint8_t expect[60] = {
    0x02, 0x93, 0xc6, 0x48, 0x4d, 0x8f, 0x02, 0x93, 0xc6, 0x48, 0x4d, 0x90, 0x08, 0x00, 0x45, 0x00,
    0x00, 0x26, 0x00, 0x00, 0x40, 0x00, 0x40, 0x11, 0xb8, 0x75, 0xc0, 0xa8, 0x00, 0xfe, 0xc0, 0xa8,
    0x00, 0x03, 0x30, 0x39, 0x20, 0x00, 0x00, 0x12, 0x00, 0x00, 0x14, 0x01, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* addr, then padding */
};

int main(void)
{
    static const uint8_t ps_mac[6] = HWHS_PS_MAC, hsb_mac1[6] = HWHS_HSB_MAC1;
    struct hwhs h;
    uint8_t f[HWHS_ECB_BUF_BYTES];

    memset(&h, 0, sizeof(h));
    memcpy(h.ps_mac, ps_mac, 6);
    memcpy(h.hsb_mac, hsb_mac1, 6);
    h.ps_ip = HWHS_PS_IP;
    h.ps_port = HWHS_PS_PORT;
    h.hsb_ip = HSB_DEFAULT_IP + 1;

    size_t n = ecb_frame(&h, f, HSB_ECB_RD_DWORD, 1, HSB_IP_VERSION, NULL);
    int bad = n != sizeof(expect);
    for (size_t i = 0; i < sizeof(expect) && i < n; i++) {
        if (f[i] != expect[i]) {
            printf("byte %zu: 0x%02x, expected 0x%02x\n", i, f[i], expect[i]);
            bad = 1;
        }
    }
    printf("%s (%zu bytes)\n", bad ? "FAIL" : "PASS", n);
    return bad;
}
