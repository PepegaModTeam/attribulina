#include "Opcodes50.h"

namespace attrib::lua::op50 {

namespace {
constexpr const char* kNames[NUM_OPCODES] = {
    "MOVE", "LOADK", "LOADBOOL", "LOADNIL", "GETUPVAL", "GETGLOBAL", "GETTABLE", "SETGLOBAL", "SETUPVAL",
    "SETTABLE", "NEWTABLE", "SELF", "ADD", "SUB", "MUL", "DIV", "POW", "UNM", "NOT", "CONCAT", "JMP", "EQ",
    "LT", "LE", "TEST", "CALL", "TAILCALL", "RETURN", "FORLOOP", "TFORLOOP", "TFORPREP", "SETLIST",
    "SETLISTO", "CLOSE", "CLOSURE",
};
} // namespace

const char* Name(Op op) { return op < NUM_OPCODES ? kNames[op] : "?"; }

Mode GetMode(Op op) {
    switch (op) {
        case LOADK: case GETGLOBAL: case SETGLOBAL: case SETLIST: case SETLISTO: case CLOSURE: return Mode::ABx;
        case JMP: case FORLOOP: case TFORPREP: return Mode::AsBx;
        default: return Mode::ABC;
    }
}

} // namespace attrib::lua::op50
