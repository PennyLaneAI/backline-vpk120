/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Xanadu Quantum Technologies Inc. */
/*
 * test_cuqr - check hsbctl's CUQR frames and endpoint parsing against cudaq-realtime's wire format.
 * Run with make test.
 */
#define main hsbctl_main
#include "../apps/hsbctl/hsbctl.c"
#undef main
#include "../lib/hwhs/hwhs.c"

static int fails;

#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "test_cuqr: %s:%d: %s\n", __FILE__, __LINE__, #c);                     \
            fails++;                                                                               \
        }                                                                                          \
    } while (0)

int main(void)
{
    /* the id and layout cudaq-realtime uses */
    CHECK(cuqr_fnv1a("rpc_increment") == CUQR_RPC_INCREMENT);
    CHECK(offsetof(struct cuqr_header, request_id) == 12);
    CHECK(offsetof(struct cuqr_response, request_id) == 12);
    CHECK(offsetof(struct cuqr_header, ptp_timestamp) == 16);

    /* round 255 of an 8-byte rpc_increment: bytes 255, 0, 1, ... and the reply wraps to 0 */
    struct frame fr = {.cuqr = 1, .function_id = CUQR_RPC_INCREMENT, .arg_len = 8};
    uint8_t q[FRAME_BYTES], r[HWHS_SLOT_BYTES] = {0};
    static const uint8_t head[24] = {0x52, 0x51, 0x55, 0x43, 0x22, 0x63, 0x1b, 0x19,
                                     0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    cuqr_request(&fr, q, 255);
    CHECK(memcmp(q, head, sizeof(head)) == 0);
    CHECK(q[24] == 255 && q[25] == 0 && q[31] == 6 && q[32] == 0);

    struct cuqr_response rh = {.magic = CUQS_MAGIC, .result_len = 8, .request_id = 7};
    memcpy(r, &rh, sizeof(rh));
    for (int k = 0; k < 8; k++) {
        r[24 + k] = (uint8_t)(q[24 + k] + 1);
    }
    CHECK(r[24] == 0);
    CHECK(cuqr_check(q, r) == 0);
    r[31] ^= 1;
    CHECK(cuqr_check(q, r) != 0);
    r[31] ^= 1;
    rh.status = -1;
    memcpy(r, &rh, sizeof(rh));
    CHECK(cuqr_check(q, r) != 0);

    /* the endpoint text from cudaq-realtime's validation docs, with an extra field */
    struct hwhs_dp dp = {0};
    char peer[64] = "";
    CHECK(parse_endpoint("qp=0x123 rkey=456 buffer_addr=0x0 peer_ip=192.168.0.2 slot_size=384 "
                         "num_slots=128 mtu=4096",
                         &dp, peer, sizeof(peer)) == 0);
    CHECK(dp.qpn == 0x123 && dp.rkey == 456 && dp.ring_iova == 0);
    CHECK(dp.ring_pitch == 384 && dp.ring_slots == 128);
    CHECK(strcmp(peer, "192.168.0.2") == 0);
    CHECK(parse_endpoint("qp=0x123 rkey=456", &dp, peer, sizeof(peer)) != 0);

    if (fails) {
        return 1;
    }
    printf("test_cuqr: ok\n");
    return 0;
}
