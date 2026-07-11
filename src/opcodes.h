#ifndef BARON_OPCODES_H_
#define BARON_OPCODES_H_

#include "richc/arena.h"   // rc_arena (opcode_parse)
#include <stdint.h>
#include <stdbool.h>


// The 6502 (NMOS) and 65C02 (CMOS) instruction set. One (mnemonic, addr_mode) -> cell table
// carries everything as flags OR'd above the opcode byte: whether the encoding is CMOS-only
// (every legal NMOS opcode is also a CMOS one, so a plain cell means "both"), and - for a
// future zero-page allocator - how that exact (mnemonic, addr_mode) touches memory and control
// flow. Encoding the class per cell (not per mnemonic) is what lets ASL A (no memory operand)
// differ from ASL zp (read+write).

typedef enum addr_mode {
    // NMOS addressing modes
    addr_mode_imp, addr_mode_acc, addr_mode_imm, addr_mode_zp,
    addr_mode_zpx, addr_mode_zpy, addr_mode_abs, addr_mode_absx,
    addr_mode_absy, addr_mode_indx, addr_mode_indy, addr_mode_ind16,
    addr_mode_rel,
    // CMOS extra addressing modes
    addr_mode_ind, addr_mode_ind16x,
    addr_mode_max
} addr_mode;

typedef enum mnemonic {
    // NMOS opcodes
    mnemonic_adc, mnemonic_and, mnemonic_asl, mnemonic_bcc,
    mnemonic_bcs, mnemonic_beq, mnemonic_bit, mnemonic_bmi,
    mnemonic_bne, mnemonic_bpl, mnemonic_brk, mnemonic_bvc,
    mnemonic_bvs, mnemonic_clc, mnemonic_cld, mnemonic_cli,
    mnemonic_clv, mnemonic_cmp, mnemonic_cpx, mnemonic_cpy,
    mnemonic_dec, mnemonic_dex, mnemonic_dey, mnemonic_eor,
    mnemonic_inc, mnemonic_inx, mnemonic_iny, mnemonic_jmp,
    mnemonic_jsr, mnemonic_lda, mnemonic_ldx, mnemonic_ldy,
    mnemonic_lsr, mnemonic_nop, mnemonic_ora, mnemonic_pha,
    mnemonic_php, mnemonic_pla, mnemonic_plp, mnemonic_rol,
    mnemonic_ror, mnemonic_rti, mnemonic_rts, mnemonic_sbc,
    mnemonic_sec, mnemonic_sed, mnemonic_sei, mnemonic_sta,
    mnemonic_stx, mnemonic_sty, mnemonic_tax, mnemonic_tay,
    mnemonic_tsx, mnemonic_txa, mnemonic_txs, mnemonic_tya,
    // CMOS extra opcodes
    mnemonic_bra, mnemonic_dea, mnemonic_ina, mnemonic_phx,
    mnemonic_phy, mnemonic_plx, mnemonic_ply, mnemonic_stz,
    mnemonic_trb, mnemonic_tsb,
    mnemonic_max
} mnemonic;

// Flags OR'd into each opcode_defs cell, above the 8-bit opcode byte.
enum opcode_flag {
    cmos = 0x100,                                                              // CMOS-only; absent = both CPUs
    op_zpread = 0x0200, op_zpwrite = 0x0400,                                   // how the ZERO-PAGE operand is
                                                                               // accessed (combine for rmw);
                                                                               // absolute carries neither
    op_branch = 0x0800, op_jump = 0x1000, op_call = 0x2000, op_return = 0x4000,// control flow (exclusive)
    op_class_mask = op_zpread | op_zpwrite | op_branch | op_jump | op_call | op_return,
};

// The raw cell for (m, mode): byte | (cmos) | class flags, or 0 when the mode is unsupported.
// So presence is `cell != 0`, the byte is `cell & 0xFF`, the encoding is CMOS-only when
// `cell & cmos` (and so unavailable on the NMOS target), and the call-analysis class is
// `cell & op_class_mask`.
uint16_t opcode_def(mnemonic m, addr_mode mode);

// Parse and assemble one instruction whose mnemonic is m. in.cursor sits just past the
// mnemonic; the result's `next` is just past the operand, with the separator consumed (or an
// error set). This lives here so opcodes.c owns both the instruction table and how to assemble
// from it. The assembler input/output and container types are forward-declared (a declaration
// may use incomplete types by value); opcodes.c includes assemble.h for the full definitions.
typedef struct baron baron;
typedef struct parse_result parse_result;
typedef struct parse_flags parse_flags;
typedef struct cursor cursor;
parse_result opcode_parse(baron *b, mnemonic m, cursor at,
                          uint32_t scope, uint32_t section, parse_flags flags, rc_arena scratch);


#endif // ifndef BARON_OPCODES_H_
