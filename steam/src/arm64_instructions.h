// Decoders and encoders for the handful of arm64 instructions the resolver and
// the patcher need.
#pragma once

#include <stdbool.h>
#include <stdint.h>

static inline int64_t arm64_sign_extend(uint64_t value, int bits) {
    uint64_t sign = 1ull << (bits - 1);
    return (int64_t)((value ^ sign) - sign);
}

// ADRP Xd, page
static inline bool arm64_adrp(uint64_t pc, uint32_t w, unsigned *rd, uint64_t *page) {
    if ((w & 0x9F000000u) != 0x90000000u)
        return false;
    uint64_t imm = ((uint64_t)((w >> 5) & 0x7FFFFu) << 2) | ((w >> 29) & 3u);
    *rd = w & 0x1Fu;
    *page = (pc & ~0xFFFull) + (uint64_t)(arm64_sign_extend(imm, 21) * 4096);
    return true;
}

// ADR Xd, addr
static inline bool arm64_adr(uint64_t pc, uint32_t w, unsigned *rd, uint64_t *addr) {
    if ((w & 0x9F000000u) != 0x10000000u)
        return false;
    uint64_t imm = ((uint64_t)((w >> 5) & 0x7FFFFu) << 2) | ((w >> 29) & 3u);
    *rd = w & 0x1Fu;
    *addr = pc + (uint64_t)arm64_sign_extend(imm, 21);
    return true;
}

// ADD Xd, Xn, #imm (64-bit, optionally shifted by 12)
static inline bool arm64_add_immediate(uint32_t w, unsigned *rd, unsigned *rn, uint64_t *imm) {
    if ((w & 0xFF800000u) != 0x91000000u)
        return false;
    *rd = w & 0x1Fu;
    *rn = (w >> 5) & 0x1Fu;
    *imm = (w >> 10) & 0xFFFu;
    if (w & (1u << 22))
        *imm <<= 12;
    return true;
}

// B or BL to target
static inline bool arm64_branch(uint64_t pc, uint32_t w, bool *link, uint64_t *target) {
    if ((w & 0x7C000000u) != 0x14000000u)
        return false;
    *link = (w & 0x80000000u) != 0;
    *target = pc + (uint64_t)(arm64_sign_extend(w & 0x03FFFFFFu, 26) * 4);
    return true;
}

// Target of any direct branch: B, BL, B.cond, CBZ/CBNZ, TBZ/TBNZ
static inline bool arm64_branch_target(uint64_t pc, uint32_t w, uint64_t *target) {
    bool link;
    if (arm64_branch(pc, w, &link, target))
        return true;
    if ((w & 0xFF000010u) == 0x54000000u || (w & 0x7E000000u) == 0x34000000u) {
        *target = pc + (uint64_t)(arm64_sign_extend((w >> 5) & 0x7FFFFu, 19) * 4);
        return true;
    }
    if ((w & 0x7E000000u) == 0x36000000u) {
        *target = pc + (uint64_t)(arm64_sign_extend((w >> 5) & 0x3FFFu, 14) * 4);
        return true;
    }
    return false;
}

// LDRB Wt, [Xn, #imm]
static inline bool arm64_ldrb(uint32_t w, unsigned *rt, unsigned *rn, uint32_t *offset) {
    if ((w & 0xFFC00000u) != 0x39400000u)
        return false;
    *rt = w & 0x1Fu;
    *rn = (w >> 5) & 0x1Fu;
    *offset = (w >> 10) & 0xFFFu;
    return true;
}

// STRB Wt, [Xn, #imm]
static inline bool arm64_strb(uint32_t w, unsigned *rt, unsigned *rn, uint32_t *offset) {
    if ((w & 0xFFC00000u) != 0x39000000u)
        return false;
    *rt = w & 0x1Fu;
    *rn = (w >> 5) & 0x1Fu;
    *offset = (w >> 10) & 0xFFFu;
    return true;
}

// CMP Wn, #imm (SUBS WZR, Wn, #imm, unshifted)
static inline bool arm64_cmp_w_immediate(uint32_t w, unsigned *rn, uint32_t *imm) {
    if ((w & 0xFFC0001Fu) != 0x7100001Fu)
        return false;
    *rn = (w >> 5) & 0x1Fu;
    *imm = (w >> 10) & 0xFFFu;
    return true;
}

// A frame-setup instruction that leaves X0 alone: SUB SP; STP/STR/STUR to SP
// or X29; ADD X29, SP; MOV Xd, Xm with Xd other than X0.
static inline bool arm64_is_frame_setup(uint32_t w) {
    unsigned rn = (w >> 5) & 0x1Fu;
    if ((w & 0xFF8003FFu) == 0xD10003FFu || (w & 0xFF8003FFu) == 0x910003FDu)
        return true;
    if (((w & 0xFFC00000u) == 0xA9000000u || (w & 0xFFC00000u) == 0xA9800000u ||
         (w & 0xFFC00000u) == 0x6D000000u || (w & 0xFFC00000u) == 0x6D800000u) && rn == 31)
        return true;
    if (((w & 0xBFC00000u) == 0xB9000000u || (w & 0xBFE00C00u) == 0xB8000000u) &&
        (rn == 31 || rn == 29))
        return true;
    return (w & 0xFFE0FFE0u) == 0xAA0003E0u && (w & 0x1Fu) != 0;
}

// Instructions whose meaning depends on where they sit, which a trampoline
// cannot copy verbatim: ADR/ADRP, B/BL, B.cond, CBZ/CBNZ, TBZ/TBNZ, LDR literal.
static inline bool arm64_is_pc_relative(uint32_t w) {
    return (w & 0x1F000000u) == 0x10000000u ||   // ADR, ADRP
           (w & 0x7C000000u) == 0x14000000u ||   // B, BL
           (w & 0xFF000010u) == 0x54000000u ||   // B.cond
           (w & 0x7E000000u) == 0x34000000u ||   // CBZ, CBNZ
           (w & 0x7E000000u) == 0x36000000u ||   // TBZ, TBNZ
           (w & 0x3B000000u) == 0x18000000u;     // LDR (literal), LDRSW, PRFM
}

// LDR X16, #8 ; BR X16 ; .quad target
static inline void arm64_write_absolute_jump(uint32_t out[4], uint64_t target) {
    out[0] = 0x58000050u;
    out[1] = 0xD61F0200u;
    out[2] = (uint32_t)target;
    out[3] = (uint32_t)(target >> 32);
}
