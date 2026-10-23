#include "Opcodes51.h"

namespace attrib::lua::op51 {

namespace {
constexpr const char* kNames[NUM_OPCODES] = {
    "MOVE", "LOADK", "LOADBOOL", "LOADNIL", "GETUPVAL", "GETGLOBAL", "GETTABLE", "SETGLOBAL", "SETUPVAL",
    "SETTABLE", "NEWTABLE", "SELF", "ADD", "SUB", "MUL", "DIV", "MOD", "POW", "UNM", "NOT", "LEN", "CONCAT",
    "JMP", "EQ", "LT", "LE", "TEST", "TESTSET", "CALL", "TAILCALL", "RETURN", "FORLOOP", "FORPREP",
    "TFORLOOP", "SETLIST", "CLOSE", "CLOSURE", "VARARG",
};
} // namespace

const char* Name(Op op) { return op < NUM_OPCODES ? kNames[op] : "?"; }

Mode GetMode(Op op) {
    switch (op) {
        case LOADK: case GETGLOBAL: case SETGLOBAL: case CLOSURE: return Mode::ABx;
        case JMP: case FORLOOP: case FORPREP: return Mode::AsBx;
        default: return Mode::ABC;
    }
}

} // namespace attrib::lua::op51
