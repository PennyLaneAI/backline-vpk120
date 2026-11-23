/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Xanadu Quantum Technologies Inc. */
/*
 * cuqr.h - cudaq-realtime's RPC frames (CUQR request, CUQS response)
 *
 *   request  (HSB writes it into a ring slot):  cuqr_header, arg_len argument bytes
 *   response (SEND back to HSB):                cuqr_response, result_len result bytes
 */
#ifndef CUQR_H
#define CUQR_H

#include <stdint.h>

#define CUQR_MAGIC          0x43555152u /* 'CUQR' */
#define CUQS_MAGIC          0x43555153u /* 'CUQS' */
#define CUQR_RPC_INCREMENT  0x191b6322u /* cuqr_fnv1a("rpc_increment"): each byte + 1 */
#define CUQR_MAX_ARG        40u         /* header + arguments in one 64 B beat */

struct __attribute__((packed)) cuqr_header {
    uint32_t magic;         /* CUQR_MAGIC */
    uint32_t function_id;   /* FNV-1a of the handler's name */
    uint32_t arg_len;       /* argument bytes after the header */
    uint32_t request_id;    /* echoed in the response; hwhs stamps its seq here (offset 12) */
    uint64_t ptp_timestamp; /* echoed; 0 here */
};
_Static_assert(sizeof(struct cuqr_header) == 24, "cuqr_header layout");

struct __attribute__((packed)) cuqr_response {
    uint32_t magic;         /* CUQS_MAGIC */
    int32_t status;         /* 0 = success; -1 = bad magic or unknown function_id */
    uint32_t result_len;    /* result bytes after the header */
    uint32_t request_id;
    uint64_t ptp_timestamp;
};
_Static_assert(sizeof(struct cuqr_response) == 24, "cuqr_response layout");

/* FNV-1a of a handler name: its function_id */
static inline uint32_t cuqr_fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

#endif /* CUQR_H */
