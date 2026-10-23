#pragma once

// Lua 5.0 instruction set (lopcodes.h): 32-bit instructions with
// OP in bits 0-5, C in 6-14, B in 15-23, A in 24-31; Bx is B:C (18 bits) and
// sBx is Bx - 131071. An RK operand >= 250 (MAXSTACK) is constant RK - 250.

#include <cstdint>

namespace attrib::lua::op50 {

enum Op : uint8_t {
    MOVE, LOADK, LOADBOOL, LOADNIL, GETUPVAL, GETGLOBAL, GETTABLE, SETGLOBAL, SETUPVAL, SETTABLE,
    NEWTABLE, SELF, ADD, SUB, MUL, DIV, POW, UNM, NOT, CONCAT, JMP, EQ, LT, LE, TEST, CALL,
    TAILCALL, RETURN, FORLOOP, TFORLOOP, TFORPREP, SETLIST, SETLISTO, CLOSE, CLOSURE,
    NUM_OPCODES
};

enum class Mode : uint8_t { ABC, ABx, AsBx };

constexpr int MAXSTACK = 250;      // RK operands at or above this are constants
constexpr int NO_REG = 255;        // MAXARG_A
constexpr int MAXARG_Bx = (1 << 18) - 1;
constexpr int MAXARG_sBx = MAXARG_Bx >> 1;
constexpr int FIELDS_PER_FLUSH = 32; // LFIELDS_PER_FLUSH

inline Op GetOp(uint32_t i) { return Op(i & 0x3F); }
inline int GetA(uint32_t i) { return int(i >> 24); }
inline int GetB(uint32_t i) { return int((i >> 15) & 0x1FF); }
inline int GetC(uint32_t i) { return int((i >> 6) & 0x1FF); }
inline int GetBx(uint32_t i) { return int((i >> 6) & 0x3FFFF); }
inline int GetSBx(uint32_t i) { return GetBx(i) - MAXARG_sBx; }

inline bool IsK(int rk) { return rk >= MAXSTACK; }
inline int KIndex(int rk) { return rk - MAXSTACK; }

const char* Name(Op op);
Mode GetMode(Op op);
// EQ, LT, LE, TEST: always followed by a JMP that they may skip.
inline bool IsTest(Op op) { return op == EQ || op == LT || op == LE || op == TEST; }

} // namespace attrib::lua::op50
