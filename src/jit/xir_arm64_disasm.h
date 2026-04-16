/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_arm64_disasm.h - ARM64 instruction disassembler for JIT debugging
 *
 * KEY CONCEPT:
 *   Decode uint32_t ARM64 instructions into human-readable text.
 *   Covers the instruction subset used by xray JIT codegen.
 *   Pure functions, no state, header-only.
 */

#ifndef XIR_ARM64_DISASM_H
#define XIR_ARM64_DISASM_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *a64d_reg(uint32_t r, int is_sp) {
    static const char *names[] = {
        "x0","x1","x2","x3","x4","x5","x6","x7",
        "x8","x9","x10","x11","x12","x13","x14","x15",
        "x16","x17","x18","x19","x20","x21","x22","x23",
        "x24","x25","x26","x27","x28","fp","lr","sp"
    };
    static const char *names_zr[] = {
        "x0","x1","x2","x3","x4","x5","x6","x7",
        "x8","x9","x10","x11","x12","x13","x14","x15",
        "x16","x17","x18","x19","x20","x21","x22","x23",
        "x24","x25","x26","x27","x28","fp","lr","xzr"
    };
    if (r > 31) return "???";
    return is_sp ? names[r] : names_zr[r];
}

static const char *a64d_wreg(uint32_t r) {
    static const char *names[] = {
        "w0","w1","w2","w3","w4","w5","w6","w7",
        "w8","w9","w10","w11","w12","w13","w14","w15",
        "w16","w17","w18","w19","w20","w21","w22","w23",
        "w24","w25","w26","w27","w28","w29","w30","wzr"
    };
    if (r > 31) return "???";
    return names[r];
}

static const char *a64d_cond(uint32_t c) {
    static const char *cc[] = {
        "eq","ne","cs","cc","mi","pl","vs","vc",
        "hi","ls","ge","lt","gt","le","al","nv"
    };
    return (c < 16) ? cc[c] : "??";
}

// Sign-extend a bitfield
static inline int64_t a64d_sext(uint64_t val, int bits) {
    uint64_t sign = (uint64_t)1 << (bits - 1);
    return (int64_t)((val ^ sign) - sign);
}

/*
 * Disassemble one ARM64 instruction into buf.
 * pc_offset: byte offset from code start (for branch target display).
 * Returns number of chars written.
 */
static int a64_disasm_one(char *buf, int bufsz, uint32_t inst, uint32_t pc_offset) {
    uint32_t rd  = inst & 0x1f;
    uint32_t rn  = (inst >> 5) & 0x1f;
    uint32_t rm  = (inst >> 16) & 0x1f;
    uint32_t opc = inst >> 24;

    // NOP
    if (inst == 0xd503201f)
        return snprintf(buf, bufsz, "nop");

    // RET
    if ((inst & 0xfffffc1f) == 0xd65f0000)
        return snprintf(buf, bufsz, "ret x%u", rn);

    // BLR Xn
    if ((inst & 0xfffffc1f) == 0xd63f0000)
        return snprintf(buf, bufsz, "blr %s", a64d_reg(rn, 0));

    // BR Xn
    if ((inst & 0xfffffc1f) == 0xd61f0000)
        return snprintf(buf, bufsz, "br %s", a64d_reg(rn, 0));

    // B imm26
    if ((inst >> 26) == 0x05) {
        int32_t off = (int32_t)a64d_sext(inst & 0x03ffffff, 26) * 4;
        return snprintf(buf, bufsz, "b 0x%x", pc_offset + off);
    }

    // BL imm26
    if ((inst >> 26) == 0x25) {
        int32_t off = (int32_t)a64d_sext(inst & 0x03ffffff, 26) * 4;
        return snprintf(buf, bufsz, "bl 0x%x", pc_offset + off);
    }

    // B.cond imm19
    if ((inst >> 24) == 0x54 && (inst & 0x10) == 0) {
        uint32_t cond = inst & 0xf;
        int32_t off = (int32_t)a64d_sext((inst >> 5) & 0x7ffff, 19) * 4;
        return snprintf(buf, bufsz, "b.%s 0x%x", a64d_cond(cond), pc_offset + off);
    }

    // CBZ/CBNZ
    if ((inst >> 24) == 0xb4 || (inst >> 24) == 0xb5) {
        int is_nz = (inst >> 24) & 1;
        int32_t off = (int32_t)a64d_sext((inst >> 5) & 0x7ffff, 19) * 4;
        return snprintf(buf, bufsz, "%s %s, 0x%x",
            is_nz ? "cbnz" : "cbz", a64d_reg(rd, 0), pc_offset + off);
    }

    /* STP/LDP (signed offset, pre-index, post-index) — GP and FP pairs
     * Addressing mode in bits[25:23]: 010=signed-offset, 011=pre-index, 001=post-index
     * opc byte has bits[25:24], bit 23 is in the second byte */
    if ((opc & 0xfe) == 0xa8 ||   // GP 64-bit (X-register pairs)
        (opc & 0xfe) == 0x28 ||   // GP 32-bit (W-register pairs)
        (opc & 0xfe) == 0x6c) {   // FP 64-bit (D-register pairs)
        int is_vfp = (inst >> 26) & 1;
        int size = (opc >> 6) & 3;
        int scale = is_vfp ? (4 << size) : ((size & 2) ? 8 : 4);
        int is_load = (inst >> 22) & 1;
        int mode = ((opc & 0x03) << 1) | ((inst >> 23) & 1);
        int is_pre  = (mode == 3);
        int is_post = (mode == 1);
        uint32_t rt2 = (inst >> 10) & 0x1f;
        int32_t imm7 = (int32_t)a64d_sext((inst >> 15) & 0x7f, 7) * scale;
        char r1b[8], r2b[8];
        if (is_vfp) {
            char p = (size == 0) ? 's' : (size == 1) ? 'd' : 'q';
            snprintf(r1b, sizeof(r1b), "%c%u", p, rd);
            snprintf(r2b, sizeof(r2b), "%c%u", p, rt2);
        }
        const char *r1 = is_vfp ? r1b : ((scale >= 8) ? a64d_reg(rd, 0) : a64d_wreg(rd));
        const char *r2t = is_vfp ? r2b : ((scale >= 8) ? a64d_reg(rt2, 0) : a64d_wreg(rt2));
        if (is_post)
            return snprintf(buf, bufsz, "%s %s, %s, [%s], #%d",
                is_load ? "ldp" : "stp", r1, r2t, a64d_reg(rn, 1), imm7);
        else if (is_pre)
            return snprintf(buf, bufsz, "%s %s, %s, [%s, #%d]!",
                is_load ? "ldp" : "stp", r1, r2t, a64d_reg(rn, 1), imm7);
        else
            return snprintf(buf, bufsz, "%s %s, %s, [%s, #%d]",
                is_load ? "ldp" : "stp", r1, r2t, a64d_reg(rn, 1), imm7);
    }

    // ADD/SUB imm12 (64-bit)
    if ((opc & 0x7f) == 0x11 || (opc & 0x7f) == 0x51) {
        int is_sub = (opc & 0x40) != 0;
        int sf = (opc >> 7) & 1;
        uint32_t imm12 = (inst >> 10) & 0xfff;
        uint32_t sh = (inst >> 22) & 1;
        if (sf)
            return snprintf(buf, bufsz, "%s %s, %s, #%u%s",
                is_sub ? "sub" : "add",
                a64d_reg(rd, 1), a64d_reg(rn, 1), imm12,
                sh ? ", lsl #12" : "");
        else
            return snprintf(buf, bufsz, "%s %s, %s, #%u%s",
                is_sub ? "sub" : "add",
                a64d_wreg(rd), a64d_wreg(rn), imm12,
                sh ? ", lsl #12" : "");
    }

    // ADDS/SUBS imm12 (includes CMP imm)
    if ((opc & 0x7f) == 0x31 || (opc & 0x7f) == 0x71) {
        int is_sub = (opc & 0x40) != 0;
        int sf = (opc >> 7) & 1;
        uint32_t imm12 = (inst >> 10) & 0xfff;
        if (rd == 31) {
            return snprintf(buf, bufsz, "cmp %s, #%u",
                sf ? a64d_reg(rn, 1) : a64d_wreg(rn), imm12);
        }
        return snprintf(buf, bufsz, "%s %s, %s, #%u",
            is_sub ? "subs" : "adds",
            sf ? a64d_reg(rd, 0) : a64d_wreg(rd),
            sf ? a64d_reg(rn, 1) : a64d_wreg(rn), imm12);
    }

    // ADD/SUB shifted reg (64-bit)
    if ((inst & 0x7f200000) == 0x0b000000) {
        int sf = (inst >> 31) & 1;
        int is_sub = (inst >> 30) & 1;
        int S = (inst >> 29) & 1;
        uint32_t shift = (inst >> 10) & 0x3f;
        uint32_t shtype = (inst >> 22) & 3;
        const char *shn[] = {"lsl","lsr","asr","???"};
        if (S && rd == 31 && is_sub) {
            if (shift)
                return snprintf(buf, bufsz, "cmp %s, %s, %s #%u",
                    sf ? a64d_reg(rn, 0) : a64d_wreg(rn),
                    sf ? a64d_reg(rm, 0) : a64d_wreg(rm),
                    shn[shtype], shift);
            return snprintf(buf, bufsz, "cmp %s, %s",
                sf ? a64d_reg(rn, 0) : a64d_wreg(rn),
                sf ? a64d_reg(rm, 0) : a64d_wreg(rm));
        }
        const char *op = is_sub ? (S ? "subs" : "sub") : (S ? "adds" : "add");
        if (shift)
            return snprintf(buf, bufsz, "%s %s, %s, %s, %s #%u", op,
                sf ? a64d_reg(rd, 0) : a64d_wreg(rd),
                sf ? a64d_reg(rn, 0) : a64d_wreg(rn),
                sf ? a64d_reg(rm, 0) : a64d_wreg(rm),
                shn[shtype], shift);
        return snprintf(buf, bufsz, "%s %s, %s, %s", op,
            sf ? a64d_reg(rd, 0) : a64d_wreg(rd),
            sf ? a64d_reg(rn, 0) : a64d_wreg(rn),
            sf ? a64d_reg(rm, 0) : a64d_wreg(rm));
    }

    // MOVZ / MOVK
    if ((inst & 0x1f800000) == 0x12800000) {
        int sf = (inst >> 31) & 1;
        int opc2 = (inst >> 29) & 3;
        uint32_t hw = (inst >> 21) & 3;
        uint32_t imm16 = (inst >> 5) & 0xffff;
        const char *mn = opc2 == 2 ? "movz" : opc2 == 3 ? "movk" : "movn";
        if (hw)
            return snprintf(buf, bufsz, "%s %s, #0x%x, lsl #%u", mn,
                sf ? a64d_reg(rd, 0) : a64d_wreg(rd), imm16, hw * 16);
        return snprintf(buf, bufsz, "%s %s, #0x%x", mn,
            sf ? a64d_reg(rd, 0) : a64d_wreg(rd), imm16);
    }

    // Logical shifted reg: AND/ORR/EOR/ANDS
    if ((inst & 0x1f000000) == 0x0a000000) {
        int sf = (inst >> 31) & 1;
        int opc2 = (inst >> 29) & 3;
        int N = (inst >> 21) & 1;
        uint32_t shift = (inst >> 10) & 0x3f;
        const char *ops[] = {"and","orr","eor","ands"};
        const char *op = ops[opc2];
        // MOV alias: ORR Xd, XZR, Xm
        if (opc2 == 1 && !N && rn == 31 && shift == 0)
            return snprintf(buf, bufsz, "mov %s, %s",
                sf ? a64d_reg(rd, 0) : a64d_wreg(rd),
                sf ? a64d_reg(rm, 0) : a64d_wreg(rm));
        // MVN alias: ORN Xd, XZR, Xm
        if (opc2 == 1 && N && rn == 31 && shift == 0)
            return snprintf(buf, bufsz, "mvn %s, %s",
                sf ? a64d_reg(rd, 0) : a64d_wreg(rd),
                sf ? a64d_reg(rm, 0) : a64d_wreg(rm));
        // TST alias: ANDS XZR, Xn, Xm
        if (opc2 == 3 && rd == 31 && shift == 0)
            return snprintf(buf, bufsz, "tst %s, %s",
                sf ? a64d_reg(rn, 0) : a64d_wreg(rn),
                sf ? a64d_reg(rm, 0) : a64d_wreg(rm));
        if (shift)
            return snprintf(buf, bufsz, "%s%s %s, %s, %s, lsl #%u",
                op, N ? "n" : "",
                sf ? a64d_reg(rd, 0) : a64d_wreg(rd),
                sf ? a64d_reg(rn, 0) : a64d_wreg(rn),
                sf ? a64d_reg(rm, 0) : a64d_wreg(rm), shift);
        return snprintf(buf, bufsz, "%s%s %s, %s, %s",
            op, N ? "n" : "",
            sf ? a64d_reg(rd, 0) : a64d_wreg(rd),
            sf ? a64d_reg(rn, 0) : a64d_wreg(rn),
            sf ? a64d_reg(rm, 0) : a64d_wreg(rm));
    }

    // MUL / SDIV / MSUB / MADD (Data Processing 2-source & 3-source)
    if ((inst & 0x7fe0fc00) == 0x1b000000) {
        // MADD: Xd = Xa + Xn*Xm; if Ra==XZR => MUL
        uint32_t ra = (inst >> 10) & 0x1f;
        int sf = (inst >> 31) & 1;
        if (ra == 31)
            return snprintf(buf, bufsz, "mul %s, %s, %s",
                sf ? a64d_reg(rd, 0) : a64d_wreg(rd),
                sf ? a64d_reg(rn, 0) : a64d_wreg(rn),
                sf ? a64d_reg(rm, 0) : a64d_wreg(rm));
        return snprintf(buf, bufsz, "madd %s, %s, %s, %s",
            sf ? a64d_reg(rd, 0) : a64d_wreg(rd),
            sf ? a64d_reg(rn, 0) : a64d_wreg(rn),
            sf ? a64d_reg(rm, 0) : a64d_wreg(rm),
            sf ? a64d_reg(ra, 0) : a64d_wreg(ra));
    }
    if ((inst & 0x7fe0fc00) == 0x1b008000) {
        // MSUB: Xd = Xa - Xn*Xm; if Ra==XZR => MNEG
        uint32_t ra = (inst >> 10) & 0x1f;
        int sf = (inst >> 31) & 1;
        if (ra == 31)
            return snprintf(buf, bufsz, "mneg %s, %s, %s",
                sf ? a64d_reg(rd, 0) : a64d_wreg(rd),
                sf ? a64d_reg(rn, 0) : a64d_wreg(rn),
                sf ? a64d_reg(rm, 0) : a64d_wreg(rm));
        return snprintf(buf, bufsz, "msub %s, %s, %s, %s",
            sf ? a64d_reg(rd, 0) : a64d_wreg(rd),
            sf ? a64d_reg(rn, 0) : a64d_wreg(rn),
            sf ? a64d_reg(rm, 0) : a64d_wreg(rm),
            sf ? a64d_reg(ra, 0) : a64d_wreg(ra));
    }
    if ((inst & 0x7fe0fc00) == 0x1ac00c00) {
        int sf = (inst >> 31) & 1;
        return snprintf(buf, bufsz, "sdiv %s, %s, %s",
            sf ? a64d_reg(rd, 0) : a64d_wreg(rd),
            sf ? a64d_reg(rn, 0) : a64d_wreg(rn),
            sf ? a64d_reg(rm, 0) : a64d_wreg(rm));
    }
    // LSLV/ASRV/LSRV
    if ((inst & 0x7fe0f000) == 0x1ac02000) {
        int sf = (inst >> 31) & 1;
        uint32_t op2 = (inst >> 10) & 3;
        const char *ops[] = {"lsl","lsr","asr","ror"};
        return snprintf(buf, bufsz, "%s %s, %s, %s", ops[op2],
            sf ? a64d_reg(rd, 0) : a64d_wreg(rd),
            sf ? a64d_reg(rn, 0) : a64d_wreg(rn),
            sf ? a64d_reg(rm, 0) : a64d_wreg(rm));
    }

    // CSEL / CSET
    if ((inst & 0x7fe00c00) == 0x1a800000) {
        int sf = (inst >> 31) & 1;
        uint32_t cond = (inst >> 12) & 0xf;
        // CSINC: if rm==rn==31 && cond!=14 => CSET
        if (rm == 31 && rn == 31 && cond != 14)
            return snprintf(buf, bufsz, "cset %s, %s",
                sf ? a64d_reg(rd, 0) : a64d_wreg(rd),
                a64d_cond(cond ^ 1));
        return snprintf(buf, bufsz, "csel %s, %s, %s, %s",
            sf ? a64d_reg(rd, 0) : a64d_wreg(rd),
            sf ? a64d_reg(rn, 0) : a64d_wreg(rn),
            sf ? a64d_reg(rm, 0) : a64d_wreg(rm),
            a64d_cond(cond));
    }

    // LDR/STR unsigned offset (64-bit)
    if ((inst & 0xffc00000) == 0xf9400000) {
        uint32_t imm12 = ((inst >> 10) & 0xfff) * 8;
        return snprintf(buf, bufsz, "ldr %s, [%s, #%u]",
            a64d_reg(rd, 0), a64d_reg(rn, 1), imm12);
    }
    if ((inst & 0xffc00000) == 0xf9000000) {
        uint32_t imm12 = ((inst >> 10) & 0xfff) * 8;
        return snprintf(buf, bufsz, "str %s, [%s, #%u]",
            a64d_reg(rd, 0), a64d_reg(rn, 1), imm12);
    }

    // LDR/STR unsigned offset (32-bit W)
    if ((inst & 0xffc00000) == 0xb9400000) {
        uint32_t imm12 = ((inst >> 10) & 0xfff) * 4;
        return snprintf(buf, bufsz, "ldr %s, [%s, #%u]",
            a64d_wreg(rd), a64d_reg(rn, 1), imm12);
    }
    if ((inst & 0xffc00000) == 0xb9000000) {
        uint32_t imm12 = ((inst >> 10) & 0xfff) * 4;
        return snprintf(buf, bufsz, "str %s, [%s, #%u]",
            a64d_wreg(rd), a64d_reg(rn, 1), imm12);
    }

    // LDRB / STRB unsigned offset
    if ((inst & 0xffc00000) == 0x39400000) {
        uint32_t imm12 = (inst >> 10) & 0xfff;
        return snprintf(buf, bufsz, "ldrb %s, [%s, #%u]",
            a64d_wreg(rd), a64d_reg(rn, 1), imm12);
    }
    if ((inst & 0xffc00000) == 0x39000000) {
        uint32_t imm12 = (inst >> 10) & 0xfff;
        return snprintf(buf, bufsz, "strb %s, [%s, #%u]",
            a64d_wreg(rd), a64d_reg(rn, 1), imm12);
    }

    // LDRH / STRH unsigned offset
    if ((inst & 0xffc00000) == 0x79400000) {
        uint32_t imm12 = ((inst >> 10) & 0xfff) * 2;
        return snprintf(buf, bufsz, "ldrh %s, [%s, #%u]",
            a64d_wreg(rd), a64d_reg(rn, 1), imm12);
    }
    if ((inst & 0xffc00000) == 0x79000000) {
        uint32_t imm12 = ((inst >> 10) & 0xfff) * 2;
        return snprintf(buf, bufsz, "strh %s, [%s, #%u]",
            a64d_wreg(rd), a64d_reg(rn, 1), imm12);
    }

    // UBFM (includes LSR imm, LSL imm, UBFX, UXTB, UXTH)
    if ((inst & 0x7f800000) == 0x53000000) {
        int sf = (inst >> 31) & 1;
        uint32_t immr = (inst >> 16) & 0x3f;
        uint32_t imms = (inst >> 10) & 0x3f;
        if (!sf && imms == 0x1f)
            return snprintf(buf, bufsz, "lsr %s, %s, #%u",
                a64d_wreg(rd), a64d_wreg(rn), immr);
        if (sf && imms == 0x3f)
            return snprintf(buf, bufsz, "lsr %s, %s, #%u",
                a64d_reg(rd, 0), a64d_reg(rn, 0), immr);
        return snprintf(buf, bufsz, "ubfm %s, %s, #%u, #%u",
            sf ? a64d_reg(rd, 0) : a64d_wreg(rd),
            sf ? a64d_reg(rn, 0) : a64d_wreg(rn), immr, imms);
    }

    // ADRP
    if ((inst & 0x9f000000) == 0x90000000) {
        uint32_t immlo = (inst >> 29) & 3;
        int32_t immhi = (int32_t)a64d_sext((inst >> 5) & 0x7ffff, 19);
        int64_t imm = ((int64_t)immhi << 14) | ((int64_t)immlo << 12);
        return snprintf(buf, bufsz, "adrp %s, #0x%llx",
            a64d_reg(rd, 0), (long long)(pc_offset + imm));
    }

    // FP: LDR/STR Dt (64-bit FP, unsigned offset)
    if ((inst & 0xffc00000) == 0xfd400000) {
        uint32_t imm12 = ((inst >> 10) & 0xfff) * 8;
        return snprintf(buf, bufsz, "ldr d%u, [%s, #%u]",
            rd, a64d_reg(rn, 1), imm12);
    }
    if ((inst & 0xffc00000) == 0xfd000000) {
        uint32_t imm12 = ((inst >> 10) & 0xfff) * 8;
        return snprintf(buf, bufsz, "str d%u, [%s, #%u]",
            rd, a64d_reg(rn, 1), imm12);
    }

    // FP: FADD/FSUB/FMUL/FDIV (double)
    if ((inst & 0xff20fc00) == 0x1e602800) {
        uint32_t fop = (inst >> 12) & 3;
        const char *ops[] = {"fadd","fsub","fmul","fdiv"};
        return snprintf(buf, bufsz, "%s d%u, d%u, d%u", ops[fop], rd, rn, rm);
    }

    // FP: FNEG Dd, Dn
    if ((inst & 0xfffffc00) == 0x1e614000)
        return snprintf(buf, bufsz, "fneg d%u, d%u", rd, rn);

    // FP: FMOV Dd, Dn
    if ((inst & 0xfffffc00) == 0x1e604000)
        return snprintf(buf, bufsz, "fmov d%u, d%u", rd, rn);

    // FP: FCMP Dn, Dm
    if ((inst & 0xffe0fc1f) == 0x1e602000)
        return snprintf(buf, bufsz, "fcmp d%u, d%u", rn, rm);

    // FP: SCVTF Dd, Xn
    if ((inst & 0xfffffc00) == 0x9e620000)
        return snprintf(buf, bufsz, "scvtf d%u, %s", rd, a64d_reg(rn, 0));

    // FP: FCVTZS Xd, Dn
    if ((inst & 0xfffffc00) == 0x9e780000)
        return snprintf(buf, bufsz, "fcvtzs %s, d%u", a64d_reg(rd, 0), rn);

    // FP: FMOV Xd, Dn (to GPR)
    if ((inst & 0xfffffc00) == 0x9e660000)
        return snprintf(buf, bufsz, "fmov %s, d%u", a64d_reg(rd, 0), rn);

    // FP: FMOV Dd, Xn (from GPR)
    if ((inst & 0xfffffc00) == 0x9e670000)
        return snprintf(buf, bufsz, "fmov d%u, %s", rd, a64d_reg(rn, 0));

    // Fallback: raw hex
    return snprintf(buf, bufsz, ".inst 0x%08x", inst);
}

/*
 * Dump a region of ARM64 code to FILE.
 * code: pointer to first instruction
 * n_inst: number of 32-bit instructions
 * base_offset: byte offset of first instruction from function start
 */
static void a64_disasm_dump(FILE *out, const uint32_t *code, uint32_t n_inst,
                            uint32_t base_offset) {
    char line[256];
    for (uint32_t i = 0; i < n_inst; i++) {
        uint32_t off = base_offset + i * 4;
        a64_disasm_one(line, sizeof(line), code[i], off);
        fprintf(out, "  %04x: %08x  %s\n", off, code[i], line);
    }
}

#endif // XIR_ARM64_DISASM_H
