#pragma once

// Lua 5.1 instruction set (lopcodes.h): 32-bit instructions with
// OP in bits 0-5, A in 6-13, C in 14-22, B in 23-31; Bx is B:C (18 bits) and
// sBx is Bx - 131071. An RK operand with bit 8 set (BITRK) is constant RK & 255.

#include <cstdint>

namespace attrib::lua::op51 {

enum Op : uint8_t {
    MOVE, LOADK, LOADBOOL, LOADNIL, GETUPVAL, GETGLOBAL, GETTABLE, SETGLOBAL, SETUPVAL, SETTABLE,
    NEWTABLE, SELF, ADD, SUB, MUL, DIV, MOD, POW, UNM, NOT, LEN, CONCAT, JMP, EQ, LT, LE, TEST, TESTSET,
    CALL, TAILCALL, RETURN, FORLOOP, FORPREP, TFORLOOP, SETLIST, CLOSE, CLOSURE, VARARG,
    NUM_OPCODES
};

enum class Mode : uint8_t { ABC, ABx, AsBx };

constexpr int BITRK = 256;          // RK operands with this bit are constants
constexpr int MAXINDEXRK = 255;     // the highest constant an RK operand reaches
constexpr int NO_REG = 255;         // MAXARG_A
constexpr int MAXARG_Bx = (1 << 18) - 1;
constexpr int MAXARG_sBx = MAXARG_Bx >> 1;
constexpr int FIELDS_PER_FLUSH = 50; // LFIELDS_PER_FLUSH

// is_vararg flags (lobject.h). A main function is VARARG_ISVARARG; a
// function with `...` also gets the old `arg` table (HASARG, NEEDSARG) unless
// it reads `...` itself, which clears NEEDSARG.
constexpr int VARARG_HASARG = 1;
constexpr int VARARG_ISVARARG = 2;
constexpr int VARARG_NEEDSARG = 4;

inline Op GetOp(uint32_t i) { return Op(i & 0x3F); }
inline int GetA(uint32_t i) { return int((i >> 6) & 0xFF); }
inline int GetB(uint32_t i) { return int(i >> 23); }
inline int GetC(uint32_t i) { return int((i >> 14) & 0x1FF); }
inline int GetBx(uint32_t i) { return int(i >> 14); }
inline int GetSBx(uint32_t i) { return GetBx(i) - MAXARG_sBx; }

inline bool IsK(int rk) { return (rk & BITRK) != 0; }
inline int KIndex(int rk) { return rk & ~BITRK; }

const char* Name(Op op);
Mode GetMode(Op op);
// EQ, LT, LE, TEST, TESTSET: always followed by a JMP that they may skip.
inline bool IsTest(Op op) { return op == EQ || op == LT || op == LE || op == TEST || op == TESTSET; }

} // namespace attrib::lua::op51
