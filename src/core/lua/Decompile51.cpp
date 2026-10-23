// Lua 5.1 decompiler for Undercover's and World's stripped chunks.
//
// A port of Decompile50.cpp to Lua 5.1.3's code generator, which keeps 5.0's
// register discipline but changes most of the shapes the decoder reads:
// TEST and TESTSET replace 5.0's TEST, FORPREP and TFORLOOP replace the
// numeric prologue and TFORPREP, a `while` keeps its condition in front of
// the body, RK constants are marked by bit 8, numerals are folded at compile
// time and only enter the constant table where an instruction uses them, and
// a function records the line of its `end` as well as of its header. The
// decompiler follows the register stack discipline (temporaries live above
// the active locals and are consumed in order), recognises the jump shapes of
// `if`, `while`, `repeat` and the two `for` loops, and rebuilds `and`/`or`
// expressions from their short-circuit jumps. Where several sources compile
// to the same bytecode it picks the most natural one. Locals and parameters
// get generated names, and each nested function is placed so its header and
// its `end` land on the lines the chunk records.
//
// Decompile51 then compiles its own output and reports, as a warning, any
// function whose bytecode doesn't come back identical.

#include "Chunk.h"
#include "Lua51.h"
#include "Opcodes51.h"

#include <algorithm>
#include <bitset>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace attrib::lua {

namespace {

using namespace op51;

// Set ATTRIB_LUA_TRACE=1 to log where decoding gives up (development aid).
bool Trace() {
    static const bool on = std::getenv("ATTRIB_LUA_TRACE") != nullptr;
    return on;
}

// ============================================================================
// Lexical helpers
// ============================================================================

bool IsReserved(std::string_view s) {
    static const char* const kWords[] = {"and",  "break", "do",    "else",   "elseif", "end",   "false",
                                         "for",  "function", "if", "in",     "local",  "nil",   "not",
                                         "or",   "repeat", "return", "then", "true",   "until", "while"};
    for (const char* w : kWords)
        if (s == w) return true;
    return false;
}

bool IsIdentifier(std::string_view s) {
    if (s.empty()) return false;
    auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    if (!alpha(s[0])) return false;
    for (char c : s)
        if (!alpha(c) && !(c >= '0' && c <= '9')) return false;
    return !IsReserved(s);
}

// A Lua 5.1 string literal (llex.c read_string's escapes).
std::string QuoteString(std::string_view s) {
    std::string out = "\"";
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\a': out += "\\a"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\v': out += "\\v"; break;
            default:
                if (c < 32 || c >= 127) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\%03u", unsigned(c));
                    out += buf;
                } else {
                    out += char(c);
                }
        }
    }
    return out + "\"";
}

// The compiler reads a numeral with strtod and stores it as a float
// (lobject.c luaO_str2d with lua_Number = float).
bool ReadsBackAs(const char* text, float v) {
    char* end = nullptr;
    const float f = float(std::strtod(text, &end));
    return end && *end == 0 && std::memcmp(&f, &v, 4) == 0;
}

// Shortest numeral for a non-negative finite float that reads back to the
// same bits; empty if there is none (inf, nan).
std::string FormatNumber(float v) {
    if (std::isnan(v)) return {};
    if (std::isinf(v)) return v > 0 ? "1e999" : std::string();
    char buf[64];
    // Whole numbers as integers while that stays short.
    if (v == std::floor(v) && std::fabs(v) < 1e15f) {
        std::snprintf(buf, sizeof buf, "%.0f", double(v));
        if (ReadsBackAs(buf, v)) return buf;
    }
    std::string exp;
    for (int prec = 1; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof buf, "%.*g", prec, double(v));
        if (!ReadsBackAs(buf, v)) continue;
        if (!std::strchr(buf, 'e')) return buf;
        if (exp.empty()) exp = buf;
    }
    return exp;
}

// NEWTABLE's sizes are "floating point bytes" (lobject.c luaO_fb2int).
int Fb2Int(int x) {
    const int e = (x >> 3) & 31;
    return e == 0 ? x : ((x & 7) + 8) << (e - 1);
}

// ============================================================================
// Syntax tree
// ============================================================================

struct FuncAst;
struct Var;
struct Expr;
struct Stmt;
using ExprP = std::shared_ptr<Expr>;
using StmtP = std::shared_ptr<Stmt>;

enum class BinOp : uint8_t { Add, Sub, Mul, Div, Mod, Pow, Concat, Eq, Ne, Lt, Le, And, Or };
enum class UnOp : uint8_t { Neg, Not, Len };

struct Var {
    std::string name;
    std::string hint;
    int reg = -1;
    bool fixed = false; // name was set by the decoder (parameters, `arg`)
};

enum class EK : uint8_t {
    Nil, True, False, Number, String, Local, Upval, Global, Index, Call, Function, Table, Binary, Unary, Paren,
    Vararg
};

struct TableItem {
    ExprP key; // null for a list item
    ExprP value;
    int pc = 0; // instruction that completed the item (source order)
};

struct Expr {
    EK k = EK::Nil;
    int kidx = -1;    // Nil/True/False/Number/String: constant index (-1 when not from the constant table)
    float num = 0;
    std::string str;  // String value, Global name
    Var* var = nullptr;
    int up = -1;
    FuncAst* fn = nullptr;
    ExprP a, b;              // Index: object/key; Binary: operands; Unary/Paren: operand; Call: function
    std::vector<ExprP> args; // Call
    bool method = false;     // Call: a is Index(obj, name), printed obj:name(...)
    bool open = false;       // Call, Vararg: keeps all its results
    BinOp op = BinOp::Add;
    UnOp uop = UnOp::Not;
    int lreg = -1, rreg = -1; // Lt/Le: operand registers when both were temporaries
    std::vector<TableItem> items;
    int concatCount = 0;      // Concat: number of operands in args
};

struct Block {
    std::vector<StmtP> stmts;
};

enum class SK : uint8_t {
    Local, Assign, Call, If, While, Repeat, NumFor, GenFor, Do, Return, Break, LocalFunction, Comment
};

struct Stmt {
    SK k = SK::Call;
    std::vector<Var*> vars;
    std::vector<ExprP> lhs;
    std::vector<ExprP> rhs;
    ExprP e;                     // Call: the call; While/Repeat: condition; LocalFunction: the function
    std::vector<ExprP> conds;    // If: one per clause
    std::vector<Block> blocks;   // If: one per clause, plus the else block
    bool hasElse = false;
    Block body;                  // loops, Do
    int stepK = -1;              // NumFor without a step: constant index of the implicit 1
    std::string text;            // Comment
    std::string warning;         // reported if the statement is printed
};

struct FuncAst {
    const Function* proto = nullptr;
    std::vector<Var*> params;
    bool vararg = false;    // `...` in the parameter list
    Var* argVar = nullptr;  // the old `arg` table (LUA_COMPAT_VARARG), a local after the parameters
    std::vector<Var*> upvals;
    Block body;
    bool handler = false;
};

// Constants no instruction refers to. The compiler adds a string literal to
// the table when it reads it, even when the expression folds away: `if "x"
// then`, `not "x"`, `"a" and b`. Numerals, booleans and nil only enter the
// table where an instruction uses them, so they are never left over.
std::vector<bool> UnreferencedConstants(const Function& f) {
    const size_t nk = f.constants.size();
    std::vector<bool> used(nk, false);
    auto use = [&](int idx) {
        if (idx >= 0 && size_t(idx) < nk) used[size_t(idx)] = true;
    };
    auto rk = [&](int x) {
        if (IsK(x)) use(KIndex(x));
    };
    size_t skip = 0;
    for (uint32_t i : f.code) {
        if (skip) { // CLOSURE upvalue pseudo-instructions, SETLIST's block number
            --skip;
            continue;
        }
        switch (GetOp(i)) {
            case LOADK: case GETGLOBAL: case SETGLOBAL: use(GetBx(i)); break;
            case GETTABLE: case SELF: rk(GetC(i)); break;
            case SETTABLE: case ADD: case SUB: case MUL: case DIV: case MOD: case POW: case EQ: case LT: case LE:
                rk(GetB(i));
                rk(GetC(i));
                break;
            case CLOSURE:
                if (size_t(GetBx(i)) < f.functions.size()) skip = f.functions[size_t(GetBx(i))].upvalues;
                break;
            case SETLIST:
                if (GetC(i) == 0) skip = 1;
                break;
            default: break;
        }
    }
    std::vector<bool> unref(nk);
    for (size_t j = 0; j < nk; ++j) unref[j] = !used[j];
    return unref;
}

ExprP MakeExpr(EK k) {
    auto e = std::make_shared<Expr>();
    e->k = k;
    return e;
}

// A call or `...`, which can produce any number of values.
bool IsMulti(const ExprP& e) { return e && (e->k == EK::Call || e->k == EK::Vararg); }
// ... and keeps all of them (an open call or VARARG with B = 0).
bool IsOpen(const ExprP& e) { return IsMulti(e) && e->open; }

StmtP MakeStmt(SK k) {
    auto s = std::make_shared<Stmt>();
    s->k = k;
    return s;
}

// ============================================================================
// Shared state of one decompilation
// ============================================================================

struct Decomp {
    std::deque<Var> vars;
    std::deque<FuncAst> funcs;
    std::vector<std::string> warnings;
    std::set<std::string> globals; // every global name the chunk uses

    Var* newVar(int reg) {
        vars.emplace_back();
        vars.back().reg = reg;
        return &vars.back();
    }
    void warn(std::string w) {
        if (std::find(warnings.begin(), warnings.end(), w) == warnings.end()) warnings.push_back(std::move(w));
    }
};

// ============================================================================
// Per-function analysis: jump targets, loops and register liveness
// ============================================================================

using RegSet = std::bitset<256>;

struct Loop {
    enum Kind { NumFor, GenFor, While, Repeat } kind;
    int start = 0;     // first instruction belonging to the loop statement
    int bodyStart = 0; // While: found when its condition is read
    int bodyEnd = 0;   // first instruction after the body (While: the JMP back)
    int exit = 0;      // first instruction after the loop
    int prep = 0;      // NumFor: the FORPREP; GenFor: the JMP to TFORLOOP
};

class Analysis {
public:
    explicit Analysis(const Function& f) : f_(f), n_(int(f.code.size())) {
        pseudo_.assign(n_, false);
        for (int pc = 0; pc < n_; ++pc) {
            if (pseudo_[pc]) continue;
            if (GetOp(f.code[pc]) == CLOSURE) {
                const int bx = GetBx(f.code[pc]);
                const int nups = bx < int(f.functions.size()) ? f.functions[bx].upvalues : 0;
                for (int i = 1; i <= nups && pc + i < n_; ++i) pseudo_[pc + i] = true;
            } else if (GetOp(f.code[pc]) == SETLIST && GetC(f.code[pc]) == 0 && pc + 1 < n_) {
                pseudo_[pc + 1] = true; // the block number of a long list
            }
        }
        findLoops();
        computeLiveness();
        firstDef_.assign(256, -1);
        for (int pc = 0; pc < n_; ++pc) {
            if (pseudo_[pc]) continue;
            const RegSet d = defs(pc);
            for (size_t r = 0; r < 256; ++r)
                if (d[r] && firstDef_[r] < 0) firstDef_[r] = pc;
        }
    }

    int size() const { return n_; }
    uint32_t code(int pc) const { return f_.code[pc]; }
    Op op(int pc) const { return GetOp(f_.code[pc]); }
    bool pseudo(int pc) const { return pseudo_[pc]; }

    // Target of a JMP/FORLOOP/FORPREP.
    int target(int pc) const { return pc + 1 + GetSBx(f_.code[pc]); }
    bool isTestAt(int pc) const {
        return pc >= 0 && pc + 1 < n_ && !pseudo_[pc] && IsTest(op(pc)) && op(pc + 1) == JMP;
    }
    // A JMP that isn't the second half of a test or of a TFORLOOP.
    bool isPlainJmp(int pc) const {
        return pc >= 0 && pc < n_ && !pseudo_[pc] && op(pc) == JMP && !(pc > 0 && isTestAt(pc - 1)) &&
               !(pc > 0 && !pseudo_[pc - 1] && op(pc - 1) == TFORLOOP);
    }
    // Positions a jump to `pc` may have been redirected to: luaK_jump appends
    // the jumps pending at its position to the new JMP's list.
    std::vector<int> chain(int pc) const {
        std::vector<int> out;
        int p = pc;
        for (int guard = 0; guard < 64; ++guard) {
            if (std::find(out.begin(), out.end(), p) != out.end()) break;
            out.push_back(p);
            if (p >= n_ || !isPlainJmp(p)) break;
            p = target(p);
        }
        return out;
    }
    bool inChain(int pc, int t) const {
        const auto c = chain(pc);
        return std::find(c.begin(), c.end(), t) != c.end();
    }

    // Whether the value `reg` holds before `pc` executes is read again after it.
    bool liveAfter(int pc, int reg) const {
        if (reg < 0 || reg >= 256) return false;
        if (defs(pc)[reg] && !conditionalDef(pc)) return false;
        return liveOut_[pc][reg];
    }
    bool liveIn(int pc, int reg) const { return pc >= 0 && pc < n_ && reg >= 0 && reg < 256 && liveIn_[pc][reg]; }

    // The first instruction that writes reg, or -1.
    int firstDef(int reg) const { return reg >= 0 && reg < 256 ? firstDef_[size_t(reg)] : -1; }

    const Loop* numForAtPrep(int pc) const { return find(numFor_, pc); }
    const Loop* genForAtPrep(int pc) const { return find(genFor_, pc); }
    // `while` and `repeat` loops starting at pc, outermost first.
    const std::vector<Loop>& loopsAt(int pc) const {
        static const std::vector<Loop> none;
        auto it = loops_.find(pc);
        return it == loops_.end() ? none : it->second;
    }

    // Registers the instruction at pc reads (a CLOSURE includes the locals it captures).
    RegSet reads(int pc) const { return pseudo_[pc] ? RegSet() : uses(pc); }

    // A JMP emitted by luaK_jump takes over the jumps pending at its position,
    // so no forward jump lands on it; only a loop jumps back to one.
    bool absorbs(int pc) const { return isPlainJmp(pc) && !backTarget_.count(pc); }

    // Plain or conditional JMPs that jump back to pc (loop ends).
    const std::vector<int>& backJumps(int pc) const {
        static const std::vector<int> none;
        auto it = backJumps_.find(pc);
        return it == backJumps_.end() ? none : it->second;
    }

    // Jumps (sources) that land on pc.
    const std::vector<int>& jumpsTo(int pc) const {
        static const std::vector<int> none;
        auto it = jumpsTo_.find(pc);
        return it == jumpsTo_.end() ? none : it->second;
    }

private:
    static const Loop* find(const std::map<int, Loop>& m, int pc) {
        auto it = m.find(pc);
        return it == m.end() ? nullptr : &it->second;
    }

    void findLoops() {
        for (int pc = 0; pc < n_; ++pc) {
            if (pseudo_[pc]) continue;
            const Op o = op(pc);
            if (o == JMP || o == FORLOOP || o == FORPREP) {
                const int t = target(pc);
                jumpsTo_[t].push_back(pc);
                if (t <= pc) backTarget_.insert(t);
            }
            if (o == LOADBOOL && GetC(code(pc))) jumpsTo_[pc + 2].push_back(pc);
            if (o == FORPREP) {
                // FORPREP jumps to its FORLOOP, which jumps back to the body.
                const int t = target(pc);
                if (t > pc && t < n_ && op(t) == FORLOOP && target(t) == pc + 1 && GetA(code(t)) == GetA(code(pc))) {
                    Loop l{Loop::NumFor};
                    l.prep = pc;
                    l.start = pc;
                    l.bodyStart = pc + 1;
                    l.bodyEnd = t;
                    l.exit = t + 1;
                    numFor_[pc] = l;
                }
                continue;
            }
            if (o != JMP) continue;
            const int t = target(pc);
            if (t > pc) {
                // A generic `for` jumps to its TFORLOOP, whose JMP goes back to the body.
                if (isPlainJmp(pc) && t + 1 < n_ && op(t) == TFORLOOP && op(t + 1) == JMP && target(t + 1) == pc + 1) {
                    Loop l{Loop::GenFor};
                    l.prep = pc;
                    l.start = pc;
                    l.bodyStart = pc + 1;
                    l.bodyEnd = t;
                    l.exit = t + 2;
                    genFor_[pc] = l;
                }
                continue;
            }
            if (pc >= 1 && !pseudo_[pc - 1] && op(pc - 1) == TFORLOOP) continue;
            backJumps_[t].push_back(pc);
        }
        // Every jump back to t ends a loop starting at t: a plain JMP ends a
        // `while` (whose condition, if any, comes first), a conditional one
        // the `until` of a `repeat`. The last one belongs to the outermost.
        for (auto& [t, srcs] : backJumps_) {
            std::vector<int> sorted = srcs;
            std::sort(sorted.rbegin(), sorted.rend());
            auto& list = loops_[t];
            for (int src : sorted) {
                Loop l{isPlainJmp(src) ? Loop::While : Loop::Repeat};
                l.start = t;
                l.bodyStart = l.kind == Loop::While ? -1 : t;
                l.bodyEnd = src;
                l.exit = src + 1;
                list.push_back(l);
            }
        }
    }

    // Registers an instruction reads / writes, for liveness.
    RegSet uses(int pc) const {
        RegSet s;
        const uint32_t i = code(pc);
        const int a = GetA(i), b = GetB(i), c = GetC(i);
        auto r = [&](int x) { if (x >= 0 && x < 256) s.set(x); };
        auto rk = [&](int x) { if (!IsK(x)) r(x); };
        auto range = [&](int from, int to) { for (int x = from; x <= to && x < 256; ++x) r(x); };
        switch (op(pc)) {
            case MOVE: r(b); break;
            case GETTABLE: r(b); rk(c); break;
            case SETGLOBAL: case SETUPVAL: r(a); break;
            case SETTABLE: r(a); rk(b); rk(c); break;
            case SELF: r(b); rk(c); break;
            case ADD: case SUB: case MUL: case DIV: case MOD: case POW: rk(b); rk(c); break;
            case UNM: case NOT: case LEN: r(b); break;
            case CONCAT: range(b, c); break;
            case EQ: case LT: case LE: rk(b); rk(c); break;
            case TEST: r(a); break;
            case TESTSET: r(b); break;
            case CALL: case TAILCALL: range(a, b ? a + b - 1 : 255); break;
            case RETURN: if (b != 1) range(a, b ? a + b - 2 : 255); break;
            case FORLOOP: case FORPREP: case TFORLOOP: range(a, a + 2); break;
            case SETLIST: range(a, b ? a + b : 255); break;
            case CLOSURE: {
                const int bx = GetBx(i);
                const int nups = bx < int(f_.functions.size()) ? f_.functions[bx].upvalues : 0;
                // Captured locals are read; capturing its own register is
                // `local function f` referring to itself.
                for (int k = 1; k <= nups && pc + k < n_; ++k)
                    if (GetOp(code(pc + k)) == MOVE && GetB(code(pc + k)) != a) r(GetB(code(pc + k)));
                break;
            }
            default: break;
        }
        return s;
    }
    RegSet defs(int pc) const {
        RegSet s;
        const uint32_t i = code(pc);
        const int a = GetA(i), b = GetB(i), c = GetC(i);
        auto range = [&](int from, int to) { for (int x = from; x <= to && x < 256; ++x) if (x >= 0) s.set(x); };
        switch (op(pc)) {
            case MOVE: case LOADK: case LOADBOOL: case GETUPVAL: case GETGLOBAL: case GETTABLE: case NEWTABLE:
            case ADD: case SUB: case MUL: case DIV: case MOD: case POW: case UNM: case NOT: case LEN: case CONCAT:
            case CLOSURE: case FORPREP:
                range(a, a);
                break;
            case FORLOOP: range(a, a); range(a + 3, a + 3); break;
            case LOADNIL: range(a, b); break;
            case SELF: range(a, a + 1); break;
            case TESTSET: range(a, a); break;
            case CALL: if (c != 1) range(a, c ? a + c - 2 : 255); break;
            case TFORLOOP: range(a + 3, a + 2 + c); break;
            case VARARG: if (b != 1) range(a, b ? a + b - 2 : 255); break;
            default: break;
        }
        return s;
    }
    // TESTSET copies R(B) into R(A) only on the way to its JMP; FORLOOP sets
    // the loop variable only when it jumps back.
    bool conditionalDef(int pc) const { return op(pc) == TESTSET || op(pc) == FORLOOP; }

    std::vector<int> successors(int pc) const {
        std::vector<int> s;
        const uint32_t i = code(pc);
        switch (op(pc)) {
            case JMP: case FORPREP: s.push_back(target(pc)); break;
            case EQ: case LT: case LE: case TEST: case TESTSET: case TFORLOOP:
                s.push_back(pc + 1);
                s.push_back(pc + 2);
                break;
            case LOADBOOL: s.push_back(GetC(i) ? pc + 2 : pc + 1); break;
            case FORLOOP: s.push_back(target(pc)); s.push_back(pc + 1); break;
            case RETURN: case TAILCALL: break;
            case CLOSURE: {
                const int bx = GetBx(i);
                const int nups = bx < int(f_.functions.size()) ? f_.functions[bx].upvalues : 0;
                s.push_back(pc + 1 + nups);
                break;
            }
            case SETLIST: s.push_back(GetC(i) == 0 ? pc + 2 : pc + 1); break;
            default: s.push_back(pc + 1); break;
        }
        s.erase(std::remove_if(s.begin(), s.end(), [&](int x) { return x < 0 || x >= n_; }), s.end());
        return s;
    }

    void computeLiveness() {
        liveIn_.assign(n_, RegSet());
        liveOut_.assign(n_, RegSet());
        std::vector<RegSet> use(n_), def(n_);
        std::vector<std::vector<int>> succ(n_);
        for (int pc = 0; pc < n_; ++pc) {
            if (pseudo_[pc]) continue;
            use[pc] = uses(pc);
            def[pc] = conditionalDef(pc) ? RegSet() : defs(pc);
            succ[pc] = successors(pc);
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (int pc = n_ - 1; pc >= 0; --pc) {
                if (pseudo_[pc]) continue;
                RegSet out;
                if (op(pc) == TESTSET && pc + 2 < n_) {
                    // TESTSET copies R(B) into R(A) only on the way to its JMP.
                    RegSet viaJmp = liveIn_[pc + 1];
                    viaJmp.reset(size_t(GetA(code(pc))));
                    out = viaJmp | liveIn_[pc + 2];
                } else if (op(pc) == FORLOOP) {
                    // The jump back sets the loop variable R(A+3).
                    RegSet back = succ[pc].empty() ? RegSet() : liveIn_[target(pc)];
                    const int a3 = GetA(code(pc)) + 3;
                    if (a3 < 256) back.reset(size_t(a3));
                    out = back;
                    if (pc + 1 < n_) out |= liveIn_[pc + 1];
                } else {
                    for (int s : succ[pc]) out |= liveIn_[s];
                }
                RegSet in = use[pc] | (out & ~def[pc]);
                if (in != liveIn_[pc] || out != liveOut_[pc]) {
                    liveIn_[pc] = in;
                    liveOut_[pc] = out;
                    changed = true;
                }
            }
        }
    }

    const Function& f_;
    int n_;
    std::vector<bool> pseudo_;
    std::vector<RegSet> liveIn_, liveOut_;
    std::vector<int> firstDef_;
    std::map<int, Loop> numFor_, genFor_;
    std::map<int, std::vector<Loop>> loops_;
    std::set<int> backTarget_;
    std::map<int, std::vector<int>> backJumps_;
    std::map<int, std::vector<int>> jumpsTo_;
};

// ============================================================================
// Decoder
// ============================================================================

struct Slot {
    int reg = 0;
    ExprP e;
    int pc = 0;          // instruction that completed the value
    int group = -1;      // results of one multi-result call share an id
    bool selfFn = false; // SELF: the method
    bool selfObj = false;// SELF: the object
    bool table = false;  // NEWTABLE still taking fields
    int nhMin = 0, nhMax = 0, named = 0, naMax = 0, listed = 0;
    bool fromLoadNil = false;
    bool noResult = false; // a call kept for none of its results: the last of a list's extra values
};

struct Machine {
    int base = 0;
    std::vector<Slot> st;
    int top() const { return base + int(st.size()); }
    Slot* at(int reg) {
        const int i = reg - base;
        return i >= 0 && i < int(st.size()) ? &st[i] : nullptr;
    }
};

// One fragment of a condition: [operand code] test JMP, or a lone JMP.
struct Frag {
    enum Kind { Test, Cmp, Always } kind = Test;
    ExprP cond;            // Test: the tested value; Cmp: the comparison (a == b, a < b, a <= b)
    bool jumpWhen = true;  // the jump is taken when cond is true (false)
    int start = -1;        // first instruction of its operand code
    int pc = -1;           // the test instruction (Always: the JMP)
    int jmp = -1;
    int target = -1;
    int testA = -1, testB = -1;
    int forced = -1;       // class when the target is both a true and a false exit: 0 T, 1 F
};

struct ExitSet {
    std::vector<int> t, f;
    bool inT(int x) const { return std::find(t.begin(), t.end(), x) != t.end(); }
    bool inF(int x) const { return std::find(f.begin(), f.end(), x) != f.end(); }
};

struct Env {
    const Loop* loop = nullptr;  // innermost loop, for `break`
    int loopAt = -1;             // the loop being decoded starts here ...
    int loopExit = -1;           // ... and ends here: only loops inside it may start at loopAt too
    bool innerLoops = false;     // try those inner loops before reading the code as statements
    int blockNact = 0;           // active locals when the current block started
    bool funcBody = false;       // the block is a function's body, which ends without CLOSE
};

class FuncDecoder {
public:
    FuncDecoder(Decomp& d, const Function& f, FuncAst& ast) : d_(d), f_(f), an_(f), ast_(ast) {}

    // Decodes the function. Two guesses can only be checked later and are
    // retried when they turn out wrong: a register taken for a temporary
    // that later code treats as occupied was a local nobody read again; a
    // local that later code reuses for temporaries had gone out of scope
    // (`do ... end` without captured locals leaves no CLOSE).
    bool run(const std::vector<Var*>& upvals) {
        for (int attempt = 0; attempt < 48; ++attempt) {
            ast_ = FuncAst{};
            gapPc_ = -1;
            gapHints_.clear();
            scopePc_ = -1;
            scopeHint_ = {-1, -1};
            scopeReg_ = -1;
            scopeCands_.clear();
            const int elided = elidedHint_;
            if (runOnce(upvals)) return true;
            // A first instruction that writes above the parameters: the
            // registers below it are locals declared without code.
            if (elidedHint_ > elided) {
                if (Trace()) std::fprintf(stderr, "retry: registers below %d are locals from the start\n", elidedHint_);
                continue;
            }
            // Without new evidence, a scope end that didn't help is replaced
            // by the next earlier candidate.
            if (scopeHint_.first < 0 && gapHints_.empty() && lastScopeReg_ >= 0) {
                scopeReg_ = lastScopeReg_;
                scopeCands_ = lastScopeCands_;
                scopeHint_ = {0, scopeReg_};
            }
            if (scopeHint_.first >= 0) {
                lastScopeReg_ = scopeReg_;
                lastScopeCands_ = scopeCands_;
                auto& tried = scopeTried_[scopeReg_];
                scopeHint_ = {-1, -1};
                for (auto it = scopeCands_.rbegin(); it != scopeCands_.rend(); ++it)
                    if (!tried.count(*it)) {
                        scopeHint_ = {*it, scopeReg_};
                        break;
                    }
                if (scopeHint_.first >= 0) {
                    for (int t : tried) scopeEnds_.erase({t, scopeReg_});
                    tried.insert(scopeHint_.first);
                }
            }
            if (Trace()) {
                for (const auto& h : gapHints_) std::fprintf(stderr, "retry: register %d is a local at %d\n", h.second, h.first);
                if (scopeHint_.first >= 0)
                    std::fprintf(stderr, "retry: locals from %d end before %d\n", scopeHint_.second, scopeHint_.first);
            }
            bool added = false;
            // The hint from the furthest point reached comes first.
            if (scopeHint_.first >= 0 && scopePc_ >= gapPc_ && scopeEnds_.insert(scopeHint_).second) added = true;
            if (!added)
                for (const auto& h : gapHints_)
                    if (forced_.insert(h).second) added = true;
            if (!added && scopeHint_.first >= 0 && scopeEnds_.insert(scopeHint_).second) added = true;
            if (!added) break;
        }
        return false;
    }

private:
    bool runOnce(const std::vector<Var*>& upvals) {
        ast_.proto = &f_;
        ast_.upvals = upvals;
        regVar_.assign(256, nullptr);
        lastConsumed_.assign(256, -1);
        localWrites_.assign(256, {});
        for (int i = 0; i < f_.params; ++i) {
            Var* v = d_.newVar(i);
            ast_.params.push_back(v);
            regVar_[i] = v;
        }
        nact_ = f_.params;
        ast_.vararg = (f_.vararg & VARARG_ISVARARG) != 0;
        if (f_.vararg & VARARG_HASARG) {
            Var* v = d_.newVar(nact_);
            v->name = "arg";
            v->fixed = true;
            ast_.argVar = v;
            regVar_[nact_++] = v;
        }
        int end = an_.size();
        if (end > 0 && GetOp(f_.code[end - 1]) == RETURN && GetB(f_.code[end - 1]) == 1)
            --end; // close_func's final `return`
        else
            d_.warn("a function does not end with the compiler's final RETURN");
        // luaK_nil emits nothing for a nil that lands in a free register
        // before a function's first instruction (`local x`, or the first
        // value of `for k in nil, ...`): the registers are nil already. A
        // loop may start there too, its label set after the nil. Registers
        // read before anything writes them, and those below them, start out
        // holding such nils.
        int nils = 0;
        if (an_.size() > 0) {
            int top = std::max(nact_, elidedHint_);
            for (int r = nact_; r < 256; ++r)
                if (an_.liveIn(0, r)) top = std::max(top, r + 1);
            nils = top - nact_;
        }
        Env env;
        env.blockNact = nact_;
        if (!decodeRange(0, end, env, ast_.body, true, nils)) return false;
        return true;
    }

public:
    // A function the decoder can't read: its header and its listing as comments.
    static void Unreadable(Decomp& d, const Function& f, FuncAst& fa, const std::vector<Var*>& ups) {
        fa = FuncAst{};
        fa.proto = &f;
        fa.upvals = ups;
        for (int i = 0; i < f.params; ++i) fa.params.push_back(d.newVar(i));
        fa.vararg = (f.vararg & VARARG_ISVARARG) != 0;
        if (f.vararg & VARARG_HASARG) {
            fa.argVar = d.newVar(f.params);
            fa.argVar->name = "arg";
            fa.argVar->fixed = true;
        }
        std::string listing;
        ListFunction51(f, "function", listing);
        auto note = MakeStmt(SK::Comment);
        note->text = "could not decompile this function; its bytecode:";
        fa.body.stmts.push_back(note);
        size_t pos = 0;
        while (pos < listing.size()) {
            const size_t nl = listing.find('\n', pos);
            auto line = MakeStmt(SK::Comment);
            line->text = listing.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            fa.body.stmts.push_back(line);
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        d.warn("could not decompile a function; its body is left as a comment");
    }

private:
    // ---- context of a statement sequence or an operand ----
    struct Ctx {
        Machine m;
        Block* out = nullptr; // null: operand code, no statements allowed
        int pc = 0;
        int end = 0;
        Env env;
        int leafReg = -1;     // operand code may finish by writing this local register
        bool leafDone = false;
        int loopStartOk = -1; // a loop starting here is the one whose condition this is
        ExprP leafValue;
    };

    struct Snapshot {
        int nact;
        std::vector<Var*> regVar;
        size_t stmts;
        Machine m;
        int pc;
        std::vector<int> lastConsumed;
        std::vector<std::vector<int>> localWrites;
        Env env;
    };
    Snapshot snap(const Ctx& c) const {
        return {nact_, regVar_, c.out ? c.out->stmts.size() : 0, c.m, c.pc, lastConsumed_, localWrites_, c.env};
    }
    void restore(Ctx& c, const Snapshot& s) {
        nact_ = s.nact;
        regVar_ = s.regVar;
        lastConsumed_ = s.lastConsumed;
        localWrites_ = s.localWrites;
        if (c.out) c.out->stmts.resize(s.stmts);
        c.m = s.m;
        c.pc = s.pc;
    }

    Ctx blockCtx(int start, int end, Block* out, const Env& env, bool funcBody = false) {
        Ctx c;
        c.m.base = nact_;
        c.out = out;
        c.pc = start;
        c.end = end;
        c.env = env;
        c.env.blockNact = nact_;
        c.env.funcBody = funcBody;
        return c;
    }
    Ctx operandCtx(int start, int end, int base, const Env& env) {
        Ctx c;
        c.m.base = base;
        c.out = nullptr;
        c.pc = start;
        c.end = end;
        c.env = env;
        return c;
    }

    uint32_t I(int pc) const { return f_.code[pc]; }

    // ---- expressions for operands ----
    ExprP constant(int idx) {
        if (idx < 0 || idx >= int(f_.constants.size())) {
            d_.warn("instruction refers to a missing constant");
            return MakeExpr(EK::Nil);
        }
        const Constant& k = f_.constants[idx];
        ExprP e;
        switch (k.type) {
            case Constant::Type::Number:
                // A negative numeral is folded into one constant (lcode.c constfolding).
                e = MakeExpr(EK::Number);
                e->num = k.number;
                break;
            case Constant::Type::String:
                e = MakeExpr(EK::String);
                e->str = k.string;
                break;
            case Constant::Type::Boolean:
                e = MakeExpr(k.boolean ? EK::True : EK::False);
                break;
            default: e = MakeExpr(EK::Nil); break;
        }
        e->kidx = idx;
        return e;
    }
    ExprP localExpr(int reg) {
        ExprP e = MakeExpr(EK::Local);
        e->var = regVar_[reg];
        if (!e->var) {
            // Shouldn't happen; keep going with a placeholder local.
            Var* v = d_.newVar(reg);
            regVar_[reg] = v;
            e->var = v;
            d_.warn("read of an undeclared register");
        }
        return e;
    }

    // ---- register stack discipline ----

    bool canPromoteBelow(const Ctx& c, int level) const {
        if (level <= c.m.base) return true;
        if (!c.out) return false;
        for (const Slot& s : c.m.st) {
            if (s.reg >= level) {
                // A multi-result call can't be split between locals and temporaries.
                if (s.group >= 0) {
                    for (const Slot& o : c.m.st)
                        if (o.group == s.group && o.reg < level) return false;
                }
                break;
            }
            if (s.selfFn || s.selfObj) return false;
            if (IsOpen(s.e)) return false;
        }
        return true;
    }

    // Turns the pending values below `level` into local declarations.
    void promote(Ctx& c, int level) {
        if (level <= c.m.base) return;
        size_t n = 0;
        while (n < c.m.st.size() && c.m.st[n].reg < level) ++n;
        size_t i = 0;
        int newBase = level;
        while (i < n) {
            Slot& s = c.m.st[i];
            auto st = MakeStmt(SK::Local);
            if (s.noResult) {
                // Not a value after all: the call statement it compiles the same as.
                st->k = SK::Call;
                st->e = s.e;
                newBase = std::min(newBase, s.reg);
                c.out->stmts.push_back(st);
                ++i;
                continue;
            }
            if (s.group >= 0) {
                size_t j = i;
                while (j < n && c.m.st[j].group == s.group) {
                    Var* v = d_.newVar(c.m.st[j].reg);
                    v->hint = Hint(s.e);
                    st->vars.push_back(v);
                    regVar_[c.m.st[j].reg] = v;
                    localWrites_[c.m.st[j].reg].clear();
                    ++j;
                }
                st->rhs.push_back(s.e);
                i = j;
            } else {
                Var* v = d_.newVar(s.reg);
                v->hint = Hint(s.e);
                st->vars.push_back(v);
                regVar_[s.reg] = v;
                localWrites_[s.reg].clear();
                if (s.e->k == EK::Function) {
                    st->k = SK::LocalFunction;
                    st->e = s.e;
                } else if (!(s.e->k == EK::Nil && s.e->kidx < 0)) {
                    st->rhs.push_back(s.e);
                }
                if (s.table && s.named < s.nhMin) st->warning = "a table constructor lost fields to later assignments";
                ++i;
            }
            c.out->stmts.push_back(st);
        }
        c.m.st.erase(c.m.st.begin(), c.m.st.begin() + n);
        nact_ = std::max(nact_, newBase);
        c.m.base = newBase;
    }

    void flush(Ctx& c) {
        if (!c.m.st.empty() && c.out && canPromoteBelow(c, c.m.top())) promote(c, c.m.top());
    }

    // Finds which registers an instruction consumes as temporaries. `reads`
    // are its register operands in evaluation order; the temporaries among
    // them must be the newest pending values, in order. Values that are read
    // again later, read out of order or never read are locals. Returns the
    // register below which everything pending must become a local, or -1.
    int resolve(Ctx& c, int pc, const std::vector<int>& reads, bool swapOk, int dest, int wantTop,
                std::vector<int> mustBeLocal = {}, int liveExempt = -1) {
        const int top = c.m.top();
        for (int r : reads)
            if (forced_.count({pc, r})) mustBeLocal.push_back(r);
        // Writing or reading above the pending values: registers in between
        // were consumed earlier but are locals.
        int gapEnd = dest > top ? dest : -1;
        for (int r : reads)
            if (r >= top) gapEnd = std::max(gapEnd, r);
        if (gapEnd > top) noteGap(pc, top, gapEnd);
        for (int level = c.m.base; level <= top; ++level) {
            if (!canPromoteBelow(c, level)) continue;
            bool ok = true;
            for (int r : mustBeLocal)
                if (r >= level && r < top) ok = false;
            if (!ok) continue;
            std::vector<int> cons;
            for (int r : reads)
                if (r >= level) cons.push_back(r);
            const int k = int(cons.size());
            for (int r : cons)
                if (r >= top) ok = false;
            if (!ok) continue;
            bool inOrder = true;
            for (int j = 0; j < k; ++j)
                if (cons[j] != top - k + j) inOrder = false;
            if (!inOrder && swapOk && k == 2 && cons[0] == top - 1 && cons[1] == top - 2) inOrder = true;
            if (!inOrder) continue;
            for (int r : cons)
                if (r != liveExempt && an_.liveAfter(pc, r)) ok = false;
            if (!ok) continue;
            const int newTop = top - k;
            if (wantTop >= 0 && newTop != wantTop) continue;
            if (dest >= 0) {
                if (dest == newTop) return level;
                // An assignment to a local; values still pending belong to a
                // multiple assignment or are locals declared before it.
                if (dest < level && (newTop == level || c.out)) return level;
                continue;
            }
            return level;
        }
        return -1;
    }

    // An instruction needs `reg` to be a temporary but it holds a local:
    // that local's scope ended before the statement that last wrote it.
    void noteScope(int pc, int reg) {
        if (pc < scopePc_ || reg < 0 || reg >= 256 || localWrites_[reg].empty()) return;
        scopePc_ = pc;
        scopeReg_ = reg;
        scopeCands_ = localWrites_[reg];
        scopeHint_ = {localWrites_[reg].back(), reg};
    }
    void noteLocalWrite(int reg) {
        auto& w = localWrites_[reg];
        if (w.empty() || w.back() != stmtStart_) w.push_back(stmtStart_);
    }
    // The same for a value still pending in `c` (it would become a local
    // declared inside the block that ends): the statement needing `reg`
    // starts with the oldest value pending above it, or at pc.
    void noteScopePending(const Ctx& c, int pc, int reg) {
        if (pc < scopePc_ || !c.out) return;
        int start = pc;
        for (const Slot& sl : c.m.st)
            if (sl.reg > reg) start = std::min(start, sl.pc);
        scopePc_ = pc;
        scopeReg_ = reg;
        scopeCands_ = {start};
        scopeHint_ = {start, reg};
    }

    // Applies a scope-end hint at statement level: the statements since the
    // first local at or above `reg` was declared become a `do ... end`.
    bool endScopeAt(Ctx& c) {
        if (!c.out || scopeEnds_.empty()) return true;
        for (const auto& [pc, reg] : scopeEnds_) {
            if (pc != c.pc || reg < c.env.blockNact) continue;
            flush(c);
            if (!c.m.st.empty()) return false;
            if (reg >= nact_) continue;
            size_t from = c.out->stmts.size();
            for (size_t j = 0; j < c.out->stmts.size() && from == c.out->stmts.size(); ++j)
                for (Var* v : c.out->stmts[j]->vars)
                    if (v->reg >= reg && (c.out->stmts[j]->k == SK::Local || c.out->stmts[j]->k == SK::LocalFunction))
                        from = j;
            if (from == c.out->stmts.size()) continue;
            auto d = MakeStmt(SK::Do);
            d->body.stmts.assign(c.out->stmts.begin() + long(from), c.out->stmts.end());
            c.out->stmts.resize(from);
            c.out->stmts.push_back(d);
            nact_ = reg;
            c.m.base = reg;
        }
        return true;
    }

    void noteGap(int pc, int from, int to) {
        // Registers in between that nothing wrote before hold the nils of
        // locals declared without code at the start (see runOnce).
        if (to > elidedHint_) {
            bool fresh = true;
            for (int r = from; r < to && fresh; ++r) {
                const int d = an_.firstDef(r);
                if (lastConsumed_[r] >= 0 || (d >= 0 && d < pc)) fresh = false;
            }
            if (fresh) {
                elidedHint_ = to;
                return;
            }
        }
        if (pc < gapPc_) return;
        std::vector<std::pair<int, int>> hints;
        for (int r = from; r < to; ++r)
            if (lastConsumed_[r] >= 0) hints.push_back({lastConsumed_[r], r});
        if (hints.empty()) return;
        gapPc_ = pc;
        gapHints_ = hints;
    }

    // Applies resolve's answer: promotes, then pops the consumed temporaries
    // and returns the operand expressions (RK constants included) in order.
    bool take(Ctx& c, int pc, const std::vector<int>& rks, bool swapOk, int dest, std::vector<ExprP>& out,
              std::vector<int>* tempRegs = nullptr, int wantTop = -1, const std::vector<int>& mustBeLocal = {},
              int liveExempt = -1) {
        std::vector<int> regs;
        for (int rk : rks)
            if (!IsK(rk)) regs.push_back(rk);
        if (c.out && dest >= c.m.base && dest < c.m.top()) dropExtraValues(c, pc, dest);
        const int level = resolve(c, pc, regs, swapOk, dest, wantTop, mustBeLocal, liveExempt);
        if (level < 0) return false;
        if (c.out && level > c.m.base) {
            // Locals declared: what remains starts the next statement.
            promote(c, level);
            stmtStart_ = c.m.st.empty() ? pc : c.m.st.front().pc;
        }
        out.clear();
        std::map<int, ExprP> popped;
        int consumed = 0;
        for (int rk : rks)
            if (!IsK(rk) && rk >= c.m.base) ++consumed;
        for (int j = 0; j < consumed; ++j) {
            Slot s = c.m.st.back();
            c.m.st.pop_back();
            popped[s.reg] = s.e;
            lastConsumed_[s.reg] = pc;
        }
        for (int rk : rks) {
            if (IsK(rk)) {
                out.push_back(constant(KIndex(rk)));
                if (tempRegs) tempRegs->push_back(-1);
            } else if (rk < c.m.base) {
                out.push_back(localExpr(rk));
                if (tempRegs) tempRegs->push_back(-1);
            } else {
                out.push_back(popped[rk]);
                if (tempRegs) tempRegs->push_back(rk);
            }
        }
        return true;
    }

    // An instruction writes over pending values nobody reads again. With a
    // value below them, they were values beyond the names of a `local`
    // (`local a = x, y`): they join that declaration. Without one, they were
    // locals whose `do ... end` ended without a CLOSE.
    void dropExtraValues(Ctx& c, int pc, int dest) {
        if (!canPromoteBelow(c, c.m.top())) return;
        std::vector<ExprP> extras;
        bool onlyCall = true;
        for (const Slot& s : c.m.st) {
            if (s.reg < dest) continue;
            if (s.selfFn || s.selfObj || an_.liveIn(pc, s.reg)) return;
            if (s.group >= 0 && !extras.empty() && extras.back() == s.e) continue;
            extras.push_back(s.e);
            onlyCall = onlyCall && s.noResult;
        }
        if (extras.empty()) return;
        if (dest == c.m.base) {
            if (onlyCall) {
                promote(c, c.m.top());
                return;
            }
            const size_t from = c.out->stmts.size();
            promote(c, c.m.top());
            auto d = MakeStmt(SK::Do);
            d->body.stmts.assign(c.out->stmts.begin() + long(from), c.out->stmts.end());
            c.out->stmts.resize(from);
            c.out->stmts.push_back(d);
            nact_ = dest;
            c.m.base = dest;
            stmtStart_ = pc;
            return;
        }
        // The declaration they join can't end in a call's several results.
        const Slot* below = c.m.at(dest - 1);
        if (!below || below->group >= 0 || below->noResult) return;
        if (onlyCall) {
            // A call with no results after the last name compiles the same as
            // the call on its own.
            promote(c, c.m.top());
            stmtStart_ = pc;
            return;
        }
        promote(c, dest);
        if (c.out->stmts.empty()) return;
        Stmt& st = *c.out->stmts.back();
        if (st.k == SK::LocalFunction) {
            st.k = SK::Local;
            st.rhs = {st.e};
            st.e = nullptr;
        }
        if (st.k != SK::Local) return;
        if (st.rhs.empty()) st.rhs.push_back(MakeExpr(EK::Nil));
        st.rhs.insert(st.rhs.end(), extras.begin(), extras.end());
        c.m.st.clear();
        stmtStart_ = pc;
    }

    // A loop's code is read apart from the values pending before it; if its
    // first instruction writes over some of them, they were extra values.
    void dropBefore(Ctx& c, int pc) {
        if (!c.out || c.m.st.empty() || pc >= an_.size() || an_.pseudo(pc)) return;
        int dest = -1;
        switch (GetOp(I(pc))) {
            case MOVE: case LOADK: case LOADBOOL: case LOADNIL: case GETUPVAL: case GETGLOBAL: case GETTABLE:
            case NEWTABLE: case SELF: case ADD: case SUB: case MUL: case DIV: case MOD: case POW: case UNM: case NOT:
            case LEN: case CONCAT: case CLOSURE: case VARARG:
                dest = GetA(I(pc));
                break;
            default: return;
        }
        if (dest >= c.m.base && dest < c.m.top()) dropExtraValues(c, pc, dest);
    }

    void push(Ctx& c, int reg, ExprP e, int pc) {
        Slot s;
        s.reg = reg;
        s.e = std::move(e);
        s.pc = pc;
        c.m.st.push_back(std::move(s));
    }

    // A value lands in `dest`: a new temporary, or an assignment to a local.
    bool deliver(Ctx& c, int dest, ExprP e, int pc) {
        if (dest == c.m.top()) {
            push(c, dest, std::move(e), pc);
            return true;
        }
        if (dest < c.m.base) {
            if (c.leafReg == dest && !c.leafDone) {
                c.leafDone = true;
                c.leafValue = std::move(e);
                return true;
            }
            if (!c.out) return false;
            noteLocalWrite(dest);
            auto st = MakeStmt(SK::Assign);
            st->lhs.push_back(localExpr(dest));
            st->rhs.push_back(std::move(e));
            return finishStore(c, st, pc);
        }
        return false;
    }

    // ---- statements ----

    bool decodeRange(int start, int end, const Env& env, Block& out, bool funcBody = false, int nils = 0) {
        const int savedNact = nact_;
        Ctx c = blockCtx(start, end, &out, env, funcBody);
        for (int r = nact_; r < nact_ + nils; ++r) {
            push(c, r, MakeExpr(EK::Nil), start);
            c.m.st.back().fromLoadNil = true;
        }
        while (c.pc < end) {
            if (!step(c)) {
                if (Trace())
                    std::fprintf(stderr, "decodeRange [%d,%d) failed at %d (%s), nact %d, pending %d\n", start, end,
                                 c.pc, Name(GetOp(I(c.pc))), nact_, int(c.m.st.size()));
                nact_ = savedNact;
                return false;
            }
        }
        if (c.pc != end) {
            nact_ = savedNact;
            return false;
        }
        closeBlock(c);
        nact_ = savedNact;
        return true;
    }

    void closeBlock(Ctx& c) { flush(c); }

    // Decodes one unit at c.pc: an expression instruction, a statement or a
    // whole control structure.
    bool step(Ctx& c) {
        const int pc = c.pc;
        if (pc >= c.end) return false;
        if (!endScopeAt(c)) return false;
        if (c.out && c.m.st.empty()) stmtStart_ = pc;
        if (c.out) {
            const auto& loops = an_.loopsAt(pc);
            if (pc != c.env.loopAt) {
                // The last jump back belongs to the outermost loop starting here.
                if (!loops.empty()) return decodeLoop(c, loops[0]);
            } else if (c.env.innerLoops) {
                // Loops inside the one being decoded may start here too
                // (`repeat while a do ... end until b`).
                for (const Loop& l : loops) {
                    if (l.exit >= c.env.loopExit || l.exit > c.end) continue;
                    const Snapshot s = snap(c);
                    if (decodeLoop(c, l)) return true;
                    restore(c, s);
                }
            }
        }
        const uint32_t i = I(pc);
        switch (GetOp(i)) {
            case JMP:
                if (an_.genForAtPrep(pc)) return decodeGenFor(c);
                return plainJump(c);
            case EQ: case LT: case LE: case TEST: case TESTSET: return conditional(c);
            case FORPREP: return decodeNumFor(c);
            case RETURN: case TAILCALL: return decodeReturn(c);
            case CLOSE: return decodeClose(c);
            case FORLOOP: case TFORLOOP: return false;
            default: return exprInstr(c);
        }
    }

    bool finishStore(Ctx& c, StmtP st, int pc) {
        // More stores from the remaining temporaries continue a multiple assignment.
        if (!c.m.st.empty() && tryMultiAssign(c, st, pc)) return true;
        flush(c);
        if (!c.m.st.empty()) return false;
        c.out->stmts.push_back(st);
        return true;
    }

    bool finishStatement(Ctx& c, StmtP st) {
        flush(c);
        if (!c.m.st.empty()) return false;
        c.out->stmts.push_back(st);
        return true;
    }

    // `t1, t2, ... = e1, e2, ...` whose first store is the instruction at
    // c.pc: the targets' table and key temporaries are evaluated first, then
    // the values, and the stores run last target first, each taking the
    // newest value (lparser.c assignment). The first store may also take a
    // constant or a local, and values beyond the targets stay unused.
    bool multiAssignAt(Ctx& c) {
        if (!c.out) return false;
        const int pc = c.pc;
        struct Store {
            int pc;
            int value;               // RK
            std::vector<int> lhsRks; // SETTABLE table and key
            ExprP target;            // without the table/key parts
            int localTarget = -1;    // MOVE: the local's register
        };
        std::vector<Store> run;
        int expectValue = -1;
        for (int q = pc; q < c.end; ++q) {
            const uint32_t i = I(q);
            Store st;
            st.pc = q;
            switch (GetOp(i)) {
                case SETGLOBAL:
                    st.value = GetA(i);
                    st.target = MakeExpr(EK::Global);
                    st.target->str = stringConst(GetBx(i));
                    st.target->kidx = GetBx(i);
                    break;
                case SETUPVAL:
                    st.value = GetA(i);
                    st.target = MakeExpr(EK::Upval);
                    st.target->up = GetB(i);
                    break;
                case SETTABLE:
                    st.value = GetC(i);
                    st.lhsRks = {GetA(i), GetB(i)};
                    st.target = MakeExpr(EK::Index);
                    break;
                case MOVE:
                    if (GetA(i) >= GetB(i) || GetB(i) < c.m.base) goto done;
                    st.value = GetB(i);
                    st.localTarget = GetA(i);
                    break;
                default: goto done;
            }
            if (!run.empty()) {
                // Later stores take the values below, newest first.
                if (IsK(st.value) || st.value < c.m.base) break;
                if (expectValue >= 0 && st.value != expectValue) break;
                expectValue = st.value - 1;
            }
            run.push_back(st);
        }
    done:
        if (run.empty()) return false;
        const int top = c.m.top();
        const int v1 = run[0].value;
        const bool v1Temp = !IsK(v1) && v1 >= c.m.base && v1 < top;
        // Values: e1..e(n-1) in consecutive registers below the first store's
        // value; with one target there must be unused extra values instead.
        int vlow, extrasFrom;
        if (run.size() >= 2) {
            const int v2 = run[1].value;
            vlow = run.back().value;
            if (v2 >= top || vlow < c.m.base) return false;
            if (v1Temp && v1 != v2 + 1) return false;
            extrasFrom = v1Temp ? v1 + 1 : v2 + 1;
        } else {
            if (!v1Temp || v1 + 1 >= top) return false;
            vlow = v1;
            extrasFrom = v1 + 1;
        }
        // Table/key temporaries of indexed targets sit right below the values.
        std::vector<int> lhsTemps;
        for (const Store& st : run)
            for (int rk : st.lhsRks)
                if (!IsK(rk) && rk >= c.m.base) lhsTemps.push_back(rk);
        std::sort(lhsTemps.begin(), lhsTemps.end());
        if (std::adjacent_find(lhsTemps.begin(), lhsTemps.end()) != lhsTemps.end()) return false;
        for (size_t j = 0; j < lhsTemps.size(); ++j)
            if (lhsTemps[j] != vlow - int(lhsTemps.size()) + int(j)) return false;
        const int level = lhsTemps.empty() ? vlow : lhsTemps.front();
        if (!canPromoteBelow(c, level)) return false;
        for (const Store& st : run)
            if (st.localTarget >= level) return false;
        // Every consumed temporary dies at its store; extra values are never read.
        for (const Store& st : run) {
            if (!IsK(st.value) && st.value >= c.m.base && an_.liveAfter(st.pc, st.value)) return false;
            for (int rk : st.lhsRks)
                if (!IsK(rk) && rk >= c.m.base && an_.liveAfter(st.pc, rk)) return false;
        }
        for (int r = extrasFrom; r < top; ++r)
            if (!c.m.at(r) || an_.liveIn(run.back().pc + 1, r)) return false;
        for (const Slot& s : c.m.st)
            if (s.reg >= level && (s.selfFn || s.selfObj || IsOpen(s.e))) return false;
        // Commit.
        promote(c, level);
        auto rkExpr = [&](int rk) -> ExprP {
            if (IsK(rk)) return constant(KIndex(rk));
            if (rk < c.m.base) return localExpr(rk);
            return c.m.at(rk)->e;
        };
        auto st = MakeStmt(SK::Assign);
        for (int j = int(run.size()) - 1; j >= 0; --j) {
            ExprP t = run[j].localTarget >= 0 ? localExpr(run[j].localTarget) : run[j].target;
            if (t->k == EK::Index) {
                t->a = rkExpr(run[j].lhsRks[0]);
                t->b = rkExpr(run[j].lhsRks[1]);
            }
            st->lhs.push_back(t);
        }
        std::vector<int> valueRegs; // temporaries holding values, in source order
        for (int j = int(run.size()) - 1; j >= 1; --j) valueRegs.push_back(run[j].value);
        if (v1Temp) valueRegs.push_back(v1);
        for (int r : valueRegs) st->rhs.push_back(c.m.at(r)->e);
        if (!v1Temp) st->rhs.push_back(rkExpr(v1));
        for (int r = extrasFrom; r < top; ++r) st->rhs.push_back(c.m.at(r)->e);
        // Trailing nils the compiler added for missing values (adjust_assign).
        if (v1Temp && extrasFrom >= top) {
            size_t fills = 0;
            for (int j = int(valueRegs.size()) - 1; j >= 1 && c.m.at(valueRegs[j])->fromLoadNil; --j) ++fills;
            st->rhs.resize(st->rhs.size() - fills);
            // A call before them kept one result: `a, b = (f())`.
            if (fills > 0 && IsMulti(st->rhs.back()) && !st->rhs.back()->open) {
                ExprP p = MakeExpr(EK::Paren);
                p->a = st->rhs.back();
                st->rhs.back() = p;
            }
        }
        st->rhs.erase(std::unique(st->rhs.begin(), st->rhs.end()), st->rhs.end());
        c.m.st.resize(size_t(level - c.m.base));
        c.out->stmts.push_back(st);
        c.pc = pc + int(run.size());
        return true;
    }

    // The first store of `x, y = a, b` was a value computed straight into the
    // local x (decoded as `x = b`); the stores after it take their values
    // from the temporaries, newest first.
    bool tryMultiAssign(Ctx& c, StmtP first, int pc) {
        if (!c.out) return false;
        std::vector<ExprP> targets{first->lhs[0]};
        std::vector<ExprP> values;
        int q = pc + 1;
        Machine m = c.m;
        std::vector<int> lhsTemps;
        while (q < c.end && !m.st.empty()) {
            const uint32_t i = I(q);
            const Op o = GetOp(i);
            const int topReg = m.top() - 1;
            int valueReg = -1;
            ExprP target;
            if (o == SETGLOBAL) {
                valueReg = GetA(i);
                target = MakeExpr(EK::Global);
                target->str = stringConst(GetBx(i));
                target->kidx = GetBx(i);
            } else if (o == SETUPVAL) {
                valueReg = GetA(i);
                target = MakeExpr(EK::Upval);
                target->up = GetB(i);
            } else if (o == MOVE && GetA(i) < m.base && GetB(i) >= m.base) {
                valueReg = GetB(i);
                target = localExpr(GetA(i));
            } else if (o == SETTABLE && !IsK(GetC(i))) {
                valueReg = GetC(i);
            } else {
                break;
            }
            if (valueReg != topReg || an_.liveAfter(q, valueReg)) break;
            Slot vs = m.st.back();
            if (vs.selfFn || vs.selfObj || IsOpen(vs.e)) break;
            m.st.pop_back();
            if (o == SETTABLE) {
                target = MakeExpr(EK::Index);
                ExprP parts[2];
                const int rks[2] = {GetA(i), GetB(i)};
                bool ok = true;
                for (int j = 0; j < 2; ++j) {
                    const int rk = rks[j];
                    if (IsK(rk)) { parts[j] = constant(KIndex(rk)); continue; }
                    if (rk < m.base) { parts[j] = localExpr(rk); continue; }
                    Slot* s = m.at(rk);
                    if (!s || an_.liveAfter(q, rk) || s->selfFn || s->selfObj) { ok = false; break; }
                    parts[j] = s->e;
                    lhsTemps.push_back(rk);
                }
                if (!ok) break;
                target->a = parts[0];
                target->b = parts[1];
            }
            targets.push_back(target);
            values.push_back(vs.e);
            ++q;
        }
        if (values.empty()) return false;
        // The LHS temporaries must sit right below the values, and nothing
        // else of this statement may remain.
        std::sort(lhsTemps.begin(), lhsTemps.end());
        lhsTemps.erase(std::unique(lhsTemps.begin(), lhsTemps.end()), lhsTemps.end());
        int lowest = m.top();
        for (int r : lhsTemps) lowest = std::min(lowest, r);
        for (int j = 0; j < int(lhsTemps.size()); ++j)
            if (lhsTemps[j] != lowest + j) return false;
        if (lowest != m.top() - int(lhsTemps.size()) && !lhsTemps.empty()) return false;
        // Commit: the slots below the LHS temporaries are locals declared before.
        const int keep = lhsTemps.empty() ? m.top() : lowest;
        c.m = m;
        c.m.st.resize(size_t(std::max(0, keep - c.m.base)));
        if (!canPromoteBelow(c, c.m.top())) return false;
        flush(c);
        auto st = MakeStmt(SK::Assign);
        for (int j = int(targets.size()) - 1; j >= 0; --j) st->lhs.push_back(targets[j]);
        for (int j = int(values.size()) - 1; j >= 0; --j) st->rhs.push_back(values[j]);
        st->rhs.push_back(first->rhs[0]);
        // The registers of one multi-result call all hold the same expression.
        st->rhs.erase(std::unique(st->rhs.begin(), st->rhs.end()), st->rhs.end());
        c.out->stmts.push_back(st);
        c.pc = q;
        return true;
    }

    std::string stringConst(int idx) {
        if (idx >= 0 && idx < int(f_.constants.size()) && f_.constants[idx].type == Constant::Type::String)
            return f_.constants[idx].string;
        d_.warn("expected a string constant");
        return "_";
    }

    bool exprInstr(Ctx& c) {
        const int pc = c.pc;
        const uint32_t i = I(pc);
        const Op o = GetOp(i);
        const int A = GetA(i), B = GetB(i), C = GetC(i);
        std::vector<ExprP> ops;
        c.pc = pc + 1; // a multiple assignment may move it further
        switch (o) {
            case MOVE: {
                if (A < B && B >= c.m.base) {
                    c.pc = pc;
                    if (multiAssignAt(c)) return true;
                    c.pc = pc + 1;
                }
                // A temporary copied to a higher register would have been a
                // local; copied lower, it's stored into a local.
                std::vector<int> mustLocal;
                if (A > B) mustLocal.push_back(B);
                else if (A >= c.m.base) mustLocal.push_back(A);
                if (!take(c, pc, {B}, false, A, ops, nullptr, -1, mustLocal)) return false;
                if (!deliver(c, A, ops[0], pc)) return false;
                break;
            }
            case LOADK:
                if (!take(c, pc, {}, false, A, ops)) return false;
                if (!deliver(c, A, constant(GetBx(i)), pc)) return false;
                break;
            case LOADBOOL:
                if (C) return false;
                if (!take(c, pc, {}, false, A, ops)) return false;
                if (!deliver(c, A, MakeExpr(B ? EK::True : EK::False), pc)) return false;
                break;
            case LOADNIL: {
                for (int r = A; r <= B; ++r) {
                    if (!take(c, pc, {}, false, r, ops)) return false;
                    const bool pushed = r == c.m.top();
                    if (!deliver(c, r, MakeExpr(EK::Nil), pc)) return false;
                    if (pushed) c.m.st.back().fromLoadNil = true;
                    if (c.pc != pc + 1) return true; // a multiple assignment consumed more
                }
                break;
            }
            case GETUPVAL: {
                if (!take(c, pc, {}, false, A, ops)) return false;
                ExprP e = MakeExpr(EK::Upval);
                e->up = B;
                if (!deliver(c, A, e, pc)) return false;
                break;
            }
            case GETGLOBAL: {
                if (!take(c, pc, {}, false, A, ops)) return false;
                ExprP e = MakeExpr(EK::Global);
                e->str = stringConst(GetBx(i));
                e->kidx = GetBx(i);
                d_.globals.insert(e->str);
                if (!deliver(c, A, e, pc)) return false;
                break;
            }
            case GETTABLE: {
                if (!take(c, pc, {B, C}, false, A, ops)) return false;
                ExprP e = MakeExpr(EK::Index);
                e->a = ops[0];
                e->b = ops[1];
                if (!deliver(c, A, e, pc)) return false;
                break;
            }
            case SETGLOBAL: case SETUPVAL: {
                if (!c.out) return false;
                c.pc = pc;
                if (multiAssignAt(c)) return true;
                c.pc = pc + 1;
                if (!take(c, pc, {A}, false, -1, ops)) return false;
                auto st = MakeStmt(SK::Assign);
                ExprP t;
                if (o == SETGLOBAL) {
                    t = MakeExpr(EK::Global);
                    t->str = stringConst(GetBx(i));
                    t->kidx = GetBx(i);
                    d_.globals.insert(t->str);
                } else {
                    t = MakeExpr(EK::Upval);
                    t->up = B;
                }
                st->lhs.push_back(t);
                st->rhs.push_back(ops[0]);
                return finishStore(c, st, pc);
            }
            case SETTABLE: {
                c.pc = pc;
                if (Slot* ts = c.m.at(A); ts && ts->table && tableField(c, *ts)) return true;
                if (c.out && multiAssignAt(c)) return true;
                c.pc = pc + 1;
                if (!c.out) return false;
                // A table constructor at the bottom becomes a local rather than `({}).x = v`.
                std::vector<int> mustLocal;
                if (Slot* ts = c.m.at(A); ts && ts->e->k == EK::Table) mustLocal.push_back(A);
                if (!take(c, pc, {A, B, C}, false, -1, ops, nullptr, -1, mustLocal)) return false;
                auto st = MakeStmt(SK::Assign);
                ExprP t = MakeExpr(EK::Index);
                t->a = ops[0];
                t->b = ops[1];
                st->lhs.push_back(t);
                st->rhs.push_back(ops[2]);
                return finishStore(c, st, pc);
            }
            case NEWTABLE: {
                // Always a new temporary: a local there had gone out of scope.
                if (A < c.m.base) noteScope(pc, A);
                if (!take(c, pc, {}, false, A, ops)) return false;
                if (A != c.m.top()) return false;
                ExprP e = MakeExpr(EK::Table);
                push(c, A, e, pc);
                Slot& s = c.m.st.back();
                s.table = true;
                // The sizes are luaO_int2fb of the list item and field
                // counts, exact below 16.
                s.naMax = Fb2Int(B);
                s.nhMin = C > 0 ? Fb2Int(C - 1) + 1 : 0;
                s.nhMax = Fb2Int(C);
                break;
            }
            case SELF: {
                if (A < c.m.base) noteScope(pc, A);
                if (!take(c, pc, {B, C}, false, A, ops)) return false;
                if (A != c.m.top()) return false;
                ExprP fn = MakeExpr(EK::Index);
                fn->a = ops[0];
                fn->b = ops[1];
                push(c, A, fn, pc);
                c.m.st.back().selfFn = true;
                push(c, A + 1, ops[0], pc);
                c.m.st.back().selfObj = true;
                break;
            }
            case ADD: case SUB: case MUL: case DIV: case MOD: case POW: {
                if (!take(c, pc, {B, C}, false, A, ops)) return false;
                ExprP e = MakeExpr(EK::Binary);
                e->op = o == ADD   ? BinOp::Add
                        : o == SUB ? BinOp::Sub
                        : o == MUL ? BinOp::Mul
                        : o == DIV ? BinOp::Div
                        : o == MOD ? BinOp::Mod
                                   : BinOp::Pow;
                e->a = ops[0];
                e->b = ops[1];
                if (!deliver(c, A, e, pc)) return false;
                break;
            }
            case UNM: case NOT: case LEN: {
                if (!take(c, pc, {B}, false, A, ops)) return false;
                ExprP e = MakeExpr(EK::Unary);
                e->uop = o == UNM ? UnOp::Neg : o == NOT ? UnOp::Not : UnOp::Len;
                e->a = ops[0];
                if (!deliver(c, A, e, pc)) return false;
                break;
            }
            case CONCAT: {
                std::vector<int> rs;
                for (int r = B; r <= C; ++r) rs.push_back(r);
                std::vector<int> tr;
                if (B < c.m.base) noteScope(pc, B);
                if (!take(c, pc, rs, false, A, ops, &tr)) return false;
                for (int t : tr)
                    if (t < 0) return false; // CONCAT operands are always fresh temporaries
                ExprP e = MakeExpr(EK::Binary);
                e->op = BinOp::Concat;
                e->args = ops;
                if (!deliver(c, A, e, pc)) return false;
                break;
            }
            case CALL: c.pc = pc; return decodeCall(c);
            case SETLIST: c.pc = pc; return setList(c);
            case CLOSURE: c.pc = pc; return decodeClosure(c);
            case VARARG: {
                // `...`: like a call's results, all of them (B = 0), one, or
                // a group of B - 1.
                if (!ast_.vararg || B == 1) return false;
                if (!take(c, pc, {}, false, A, ops)) return false;
                ExprP e = MakeExpr(EK::Vararg);
                if (B == 2) {
                    if (!deliver(c, A, e, pc)) return false;
                    break;
                }
                if (A != c.m.top()) return false;
                if (B == 0) {
                    e->open = true;
                    push(c, A, e, pc);
                } else {
                    const int group = groupId_++;
                    for (int r = 0; r < B - 1; ++r) {
                        push(c, A + r, e, pc);
                        c.m.st.back().group = group;
                    }
                }
                break;
            }
            default: return false;
        }
        return true;
    }

    // SETTABLE into a table constructor still on the stack: a field.
    bool tableField(Ctx& c, Slot& ts) {
        const int pc = c.pc;
        const uint32_t i = I(pc);
        const int B = GetB(i), C = GetC(i);
        const bool atBottom = ts.reg == c.m.base;
        // The table's register stays pending (list items not yet stored may sit
        // above it); key and value temporaries must be the newest values.
        // Every field, `name = v` or `[k] = v`, is counted in NEWTABLE's size
        // hint; a store past that count was an assignment after the constructor.
        if (ts.named >= ts.nhMax && atBottom && c.out) return false;
        std::vector<int> regs; // key and value temporaries (locals are just read)
        if (!IsK(B) && B >= c.m.base) regs.push_back(B);
        if (!IsK(C) && C >= c.m.base) regs.push_back(C);
        const int top = c.m.top();
        const int k = int(regs.size());
        for (int j = 0; j < k; ++j)
            if (regs[j] != top - k + j || regs[j] <= ts.reg) return false;
        for (int r : regs)
            if (an_.liveAfter(pc, r)) return false;
        ExprP key, value;
        std::map<int, ExprP> popped;
        for (int j = 0; j < k; ++j) {
            popped[c.m.st.back().reg] = c.m.st.back().e;
            c.m.st.pop_back();
        }
        key = IsK(B) ? constant(KIndex(B)) : B < c.m.base ? localExpr(B) : popped[B];
        value = IsK(C) ? constant(KIndex(C)) : C < c.m.base ? localExpr(C) : popped[C];
        Slot& t = *c.m.at(ts.reg);
        TableItem item;
        item.key = key;
        item.value = value;
        item.pc = pc;
        ++t.named;
        t.e->items.push_back(item);
        c.pc = pc + 1;
        return true;
    }

    bool setList(Ctx& c) {
        const int pc = c.pc;
        const uint32_t i = I(pc);
        const int A = GetA(i), B = GetB(i), C = GetC(i);
        Slot* ts = c.m.at(A);
        if (!ts || !ts->table) return false;
        const int tsIndex = A - c.m.base;
        // B items (0: up to the top, the last an open call or `...`) for
        // block C of FIELDS_PER_FLUSH; C = 0 puts the block in the next word.
        const int pending = int(c.m.st.size()) - tsIndex - 1;
        const int count = B ? B : pending;
        if (pending != count || count == 0) return false;
        int block = C;
        if (C == 0) {
            if (pc + 1 >= an_.size()) return false;
            block = int(I(pc + 1));
        }
        if ((block - 1) * FIELDS_PER_FLUSH != ts->listed) return false;
        for (int j = 0; j < count; ++j) {
            const Slot& s = c.m.st[tsIndex + 1 + j];
            if (s.selfFn || s.selfObj || s.group >= 0) return false;
            if (an_.liveAfter(pc, s.reg)) return false;
            if (IsOpen(s.e) != (B == 0 && j == count - 1)) return false;
        }
        std::vector<Slot> items(c.m.st.begin() + tsIndex + 1, c.m.st.end());
        c.m.st.resize(tsIndex + 1);
        Slot& t = c.m.st.back();
        for (auto& s : items) {
            TableItem item;
            item.value = s.e;
            item.pc = s.pc;
            t.e->items.push_back(item);
            ++t.listed;
        }
        std::stable_sort(t.e->items.begin(), t.e->items.end(),
                         [](const TableItem& x, const TableItem& y) { return x.pc < y.pc; });
        c.pc = pc + (C == 0 ? 2 : 1);
        return true;
    }

    bool decodeCall(Ctx& c) {
        const int pc = c.pc;
        const uint32_t i = I(pc);
        const int A = GetA(i), B = GetB(i), C = GetC(i);
        Slot* fs = c.m.at(A);
        if (!fs) return false;
        const bool method = fs->selfFn;
        int last;
        if (B == 0) {
            if (c.m.st.empty()) return false;
            const Slot& t = c.m.st.back();
            if (!IsOpen(t.e)) return false;
            last = t.reg;
        } else {
            last = A + B - 1;
        }
        std::vector<int> rs;
        for (int r = A; r <= last; ++r) rs.push_back(r);
        std::vector<ExprP> ops;
        std::vector<int> tr;
        if (A < c.m.base) noteScope(pc, A);
        if (!take(c, pc, rs, false, -1, ops, &tr)) return false;
        if (tr.empty() || tr[0] != A) return false;
        ExprP call = MakeExpr(EK::Call);
        call->a = ops[0];
        call->method = method;
        for (size_t j = method ? 2 : 1; j < ops.size(); ++j) call->args.push_back(ops[j]);
        if (method && (call->a->k != EK::Index || !call->a->b || call->a->b->k != EK::String)) return false;
        c.pc = pc + 1;
        if (C == 1) {
            if (!c.out) return false;
            // In `x = a, f()` the call beyond the targets keeps no result and
            // the store of `a` follows.
            if (!c.m.st.empty() && A == c.m.top() && storesFrom(pc + 1, A - 1)) {
                push(c, A, call, pc);
                return true;
            }
            // In `local a = x, y, f()` the call beyond the names keeps no
            // result either; the value below it is dead.
            if (const Slot* below = c.m.at(A - 1);
                below && A == c.m.top() && !below->selfFn && !below->selfObj && !IsOpen(below->e) &&
                !an_.liveIn(pc + 1, A - 1)) {
                push(c, A, call, pc);
                c.m.st.back().noResult = true;
                return true;
            }
            auto st = MakeStmt(SK::Call);
            st->e = call;
            return finishStatement(c, st);
        }
        if (A != c.m.top()) return false;
        if (C == 0) {
            call->open = true;
            push(c, A, call, pc);
        } else if (C == 2) {
            push(c, A, call, pc);
        } else {
            const int group = groupId_++;
            for (int r = 0; r < C - 1; ++r) {
                push(c, A + r, call, pc);
                c.m.st.back().group = group;
            }
        }
        return true;
    }

    // Whether the instruction at q stores register r (SETGLOBAL, SETUPVAL,
    // SETTABLE value, MOVE into a local).
    bool storesFrom(int q, int r) const {
        if (q >= an_.size()) return false;
        const uint32_t i = I(q);
        switch (GetOp(i)) {
            case SETGLOBAL: case SETUPVAL: return GetA(i) == r;
            case SETTABLE: return GetC(i) == r;
            case MOVE: return GetB(i) == r && GetA(i) < r;
            default: return false;
        }
    }

    bool decodeClosure(Ctx& c) {
        const int pc = c.pc;
        const uint32_t i = I(pc);
        const int A = GetA(i), bx = GetBx(i);
        if (bx >= int(f_.functions.size())) return false;
        const Function& child = f_.functions[bx];
        std::vector<int> captured;
        bool selfCapture = false;
        for (int u = 0; u < child.upvalues; ++u) {
            if (pc + 1 + u >= an_.size()) return false;
            const uint32_t ui = I(pc + 1 + u);
            if (GetOp(ui) == MOVE) {
                if (GetB(ui) == A) selfCapture = true;
                else captured.push_back(GetB(ui));
            } else if (GetOp(ui) != GETUPVAL) {
                return false;
            }
        }
        std::vector<ExprP> ops;
        if (selfCapture) {
            // `local function f` declares f before its body.
            if (!c.out) return false;
            if (!take(c, pc, {}, false, A, ops, nullptr, -1, captured)) return false;
            if (A != c.m.top()) return false;
            flush(c);
            if (!c.m.st.empty()) return false;
            Var* v = d_.newVar(A);
            v->hint = "fn";
            regVar_[A] = v;
            nact_ = A + 1;
            c.m.base = nact_;
        } else {
            if (!take(c, pc, {}, false, A, ops, nullptr, -1, captured)) return false;
        }
        std::vector<Var*> ups;
        for (int u = 0; u < child.upvalues; ++u) {
            const uint32_t ui = I(pc + 1 + u);
            if (GetOp(ui) == MOVE) {
                const int r = GetB(ui);
                if (r >= nact_ || !regVar_[r]) return false;
                ups.push_back(regVar_[r]);
            } else {
                const int b = GetB(ui);
                ups.push_back(b < int(ast_.upvals.size()) ? ast_.upvals[b] : nullptr);
            }
        }
        d_.funcs.emplace_back();
        FuncAst* fa = &d_.funcs.back();
        FuncDecoder sub(d_, child, *fa);
        if (!sub.run(ups)) Unreadable(d_, child, *fa, ups);
        ExprP e = MakeExpr(EK::Function);
        e->fn = fa;
        c.pc = pc + 1 + child.upvalues;
        if (selfCapture) {
            auto st = MakeStmt(SK::LocalFunction);
            st->vars.push_back(regVar_[A]);
            st->e = e;
            c.out->stmts.push_back(st);
            return true;
        }
        return deliver(c, A, e, pc);
    }

    bool decodeReturn(Ctx& c) {
        if (!c.out) return false;
        const int pc = c.pc;
        const uint32_t i = I(pc);
        const int A = GetA(i), B = GetB(i);
        auto st = MakeStmt(SK::Return);
        std::vector<ExprP> ops;
        if (GetOp(i) == TAILCALL) {
            // `return f(...)`: TAILCALL then RETURN A 0.
            if (!decodeCallLike(c, pc, st)) return false;
            c.pc = pc + 1;
            if (c.pc < an_.size() && GetOp(I(c.pc)) == RETURN && GetB(I(c.pc)) == 0) ++c.pc;
        } else if (B == 1) {
            flush(c);
            c.pc = pc + 1;
        } else {
            int last;
            if (B == 0) {
                if (c.m.st.empty()) return false;
                const Slot& t = c.m.st.back();
                if (!IsOpen(t.e)) return false;
                last = t.reg;
            } else {
                last = A + B - 2;
            }
            // Several values are always fresh temporaries.
            if (B != 2 && A < c.m.base) noteScope(pc, A);
            std::vector<int> rs;
            for (int r = A; r <= last; ++r) rs.push_back(r);
            if (!take(c, pc, rs, false, -1, ops)) return false;
            st->rhs = ops;
            c.pc = pc + 1;
        }
        flush(c);
        if (!c.m.st.empty()) return false;
        c.out->stmts.push_back(st);
        return true;
    }

    bool decodeCallLike(Ctx& c, int pc, StmtP st) {
        const uint32_t i = I(pc);
        const int A = GetA(i), B = GetB(i);
        Slot* fs = c.m.at(A);
        if (!fs) return false;
        const bool method = fs->selfFn;
        int last;
        if (B == 0) {
            if (c.m.st.empty()) return false;
            const Slot& t = c.m.st.back();
            if (!IsOpen(t.e)) return false;
            last = t.reg;
        } else {
            last = A + B - 1;
        }
        std::vector<int> rs;
        for (int r = A; r <= last; ++r) rs.push_back(r);
        std::vector<ExprP> ops;
        std::vector<int> tr;
        if (A < c.m.base) noteScope(pc, A);
        if (!take(c, pc, rs, false, -1, ops, &tr)) return false;
        if (tr.empty() || tr[0] != A) return false;
        ExprP call = MakeExpr(EK::Call);
        call->a = ops[0];
        call->method = method;
        call->open = true;
        for (size_t j = method ? 2 : 1; j < ops.size(); ++j) call->args.push_back(ops[j]);
        st->rhs.push_back(call);
        return true;
    }

    bool decodeClose(Ctx& c) {
        if (!c.out) return false;
        const int A = GetA(I(c.pc));
        flush(c);
        if (!c.m.st.empty()) return false;
        if (c.pc + 1 < an_.size() && an_.isPlainJmp(c.pc + 1) && c.env.loop &&
            an_.inChain(c.env.loop->exit, an_.target(c.pc + 1))) {
            // CLOSE before `break`: the compiler adds it again.
            ++c.pc;
            return true;
        }
        if (A >= nact_) {
            ++c.pc;
            return true;
        }
        if (A <= c.env.blockNact && !c.env.funcBody) {
            // The block's own CLOSE; the compiler emits it for captured locals.
            ++c.pc;
            return true;
        }
        // Locals from A up were declared in a `do ... end` that ends here.
        size_t from = c.out->stmts.size();
        for (size_t j = 0; j < c.out->stmts.size(); ++j) {
            const StmtP& s = c.out->stmts[j];
            bool declares = false;
            for (Var* v : s->vars)
                if (v->reg >= A && (s->k == SK::Local || s->k == SK::LocalFunction)) declares = true;
            if (declares) {
                from = j;
                break;
            }
        }
        if (from == c.out->stmts.size()) return false;
        auto d = MakeStmt(SK::Do);
        d->body.stmts.assign(c.out->stmts.begin() + from, c.out->stmts.end());
        c.out->stmts.resize(from);
        c.out->stmts.push_back(d);
        nact_ = A;
        c.m.base = A;
        ++c.pc;
        return true;
    }

    bool plainJump(Ctx& c) {
        if (!c.out) return false;
        const int pc = c.pc;
        const int t = an_.target(pc);
        if (t > pc) {
            const Snapshot s = snap(c);
            ValueCond vc;
            if (tryValueCond(c, vc)) return true;
            restore(c, s);
        }
        flush(c);
        if (!c.m.st.empty()) return false;
        if (c.env.loop && an_.inChain(c.env.loop->exit, t)) {
            c.out->stmts.push_back(MakeStmt(SK::Break));
            c.pc = pc + 1;
            return true;
        }
        // `if false then ... end`: a jump over dead code, which may end in
        // the jump over an `else`. At the end of a `while` body the jump is
        // threaded through the loop's own jump back.
        if ((t > pc && t <= c.end) || (t <= pc && c.env.loop && c.env.loop->kind == Loop::While &&
                                       t == c.env.loop->start && pc < c.env.loop->bodyEnd)) {
            auto st = MakeStmt(SK::If);
            int next = -1;
            if (!ifBody(c, MakeExpr(EK::False), pc + 1, t, *st, next)) return false;
            c.out->stmts.push_back(st);
            c.pc = next;
            return true;
        }
        return false;
    }

    // ---- conditions ----

    // The fragment at pc: a test (operands from c.m) or a lone JMP.
    bool fragAt(Ctx& c, int pc, Frag& fr, int valueReg = -1) {
        if (an_.isPlainJmp(pc)) {
            fr.kind = Frag::Always;
            fr.pc = pc;
            fr.jmp = pc;
            fr.target = an_.target(pc);
            return c.m.st.empty();
        }
        return makeFrag(c, pc, fr, -1, {}, valueReg);
    }

    // Reads the fragment whose test is at pc; its operands are the newest values of c.m.
    // valueReg: in value context, the result register; a TEST that copies its
    // operand there on the jump keeps the value alive through the copy.
    bool makeFrag(Ctx& c, int pc, Frag& fr, int wantTop = -1, const std::vector<int>& mustBeLocal = {},
                  int valueReg = -1) {
        const uint32_t i = I(pc);
        const Op o = GetOp(i);
        const int A = GetA(i), B = GetB(i), C = GetC(i);
        fr.pc = pc;
        fr.jmp = pc + 1;
        fr.target = an_.target(pc + 1);
        std::vector<ExprP> ops;
        if (o == TEST || o == TESTSET) {
            // TEST A C tests R(A); TESTSET A B C tests R(B) and copies it to
            // R(A) on the jump. As 5.0's TEST A B C, TEST is A = B.
            const int a = A, b = o == TEST ? A : B;
            const int exempt = (valueReg >= 0 && a == b && a == valueReg) ? b : -1;
            // The compiler folds a test of most constants (goiftrue and
            // goiffalse), so a tested constant was in a local.
            std::vector<int> locals = mustBeLocal;
            if (const Slot* s = c.m.at(b); s && Untestable(s->e, C != 0)) locals.push_back(b);
            if (!take(c, pc, {b}, false, -1, ops, nullptr, wantTop, locals, exempt)) return false;
            fr.kind = Frag::Test;
            fr.cond = ops[0];
            fr.jumpWhen = C != 0;
            fr.testA = a;
            fr.testB = b;
            return true;
        }
        std::vector<int> tr;
        if (!take(c, pc, {B, C}, o != EQ, -1, ops, &tr, wantTop, mustBeLocal)) return false;
        fr.kind = Frag::Cmp;
        ExprP e = MakeExpr(EK::Binary);
        e->op = o == EQ ? BinOp::Eq : o == LT ? BinOp::Lt : BinOp::Le;
        e->a = ops[0];
        e->b = ops[1];
        e->lreg = tr[0];
        e->rreg = tr[1];
        fr.cond = e;
        fr.jumpWhen = A != 0;
        return true;
    }

    // Constants that never reach a TEST jumping when they are true (`or`,
    // goiffalse) or false (`and`, goiftrue): only strings and numbers before
    // `or`, and nil before `and`, are tested.
    static bool Untestable(const ExprP& e, bool jumpWhenTrue) {
        if (!e) return false;
        switch (e->k) {
            case EK::True: case EK::False: return true;
            case EK::Number: case EK::String: return !jumpWhenTrue;
            case EK::Nil: return jumpWhenTrue;
            default: return false;
        }
    }

    static ExprP Negate(const ExprP& e) {
        if (e->k == EK::Binary && (e->op == BinOp::Eq || e->op == BinOp::Ne)) {
            auto n = std::make_shared<Expr>(*e);
            n->op = e->op == BinOp::Eq ? BinOp::Ne : BinOp::Eq;
            return n;
        }
        if (e->k == EK::Unary && e->uop == UnOp::Not) return e->a;
        if (e->k == EK::True) return MakeExpr(EK::False);
        if (e->k == EK::False) return MakeExpr(EK::True);
        ExprP n = MakeExpr(EK::Unary);
        n->uop = UnOp::Not;
        n->a = e;
        return n;
    }

    enum class Cls { T, F, S, Internal, Bad };

    // Whether a fragment's jump means "the expression is true" or "false",
    // continues at the next operand (S), or goes elsewhere.
    Cls classify(const Frag& fr, const ExitSet& ex, int s, const std::vector<Frag>& frags, int from, int to) const {
        const int t = fr.target;
        if (t == s) return Cls::S;
        const bool inT = ex.inT(t), inF = ex.inF(t);
        if (inT && inF) {
            if (fr.forced >= 0) return fr.forced == 0 ? Cls::T : Cls::F;
            if (fr.kind == Frag::Always) return Cls::T;
            // A TEST of a NOT's result stands for `not not x` (see leafOf).
            if (fr.kind == Frag::Test && IsNot(fr.cond)) return fr.jumpWhen ? Cls::F : Cls::T;
            return fr.jumpWhen ? Cls::T : Cls::F;
        }
        if (inT) return Cls::T;
        if (inF) return Cls::F;
        for (int m = from + 1; m < to; ++m)
            if (frags[m].start == t) return Cls::Internal;
        return Cls::Bad;
    }

    static bool IsNot(const ExprP& e) { return e->k == EK::Unary && e->uop == UnOp::Not; }

    ExprP leafOf(const Frag& fr, Cls cls) const {
        if (fr.kind == Frag::Always) return MakeExpr(cls == Cls::T ? EK::True : EK::False);
        // The compiler drops a `not` right before a test (lcode.c jumponcond),
        // so a NOT still in the code was under a second `not`.
        if (fr.kind == Frag::Test && IsNot(fr.cond) && fr.jumpWhen == (cls == Cls::F)) {
            ExprP n = MakeExpr(EK::Unary);
            n->uop = UnOp::Not;
            n->a = fr.cond;
            return n;
        }
        const bool negate = (cls == Cls::T) != fr.jumpWhen;
        if (!negate) return fr.cond;
        // `not <constant>` is folded by the compiler: no such source.
        if (fr.kind == Frag::Test && IsConstant(fr.cond)) return nullptr;
        return Negate(fr.cond);
    }

    static bool IsConstant(const ExprP& e) {
        return e->k == EK::Nil || e->k == EK::True || e->k == EK::False || e->k == EK::Number ||
               e->k == EK::String;
    }

    // Rebuilds the and/or expression of frags[from, to) (plus a trailing
    // value `leaf` in value context) from where the jumps go.
    ExprP build(const std::vector<Frag>& frags, int from, int to, const ExprP& leaf, int leafStart,
                const ExitSet& ex) {
        if (from == to) return leaf;
        if (to - from == 1 && !leaf) {
            const Cls cl = classify(frags[from], ex, -1, frags, from, to);
            if (cl != Cls::T && cl != Cls::F) return nullptr;
            return leafOf(frags[from], cl); // may be null
        }
        for (int k = leaf ? to : to - 1; k > from; --k) {
            const int s = k < to ? frags[k].start : leafStart;
            // X = frags[from, k): its jumps go to s, to an outer exit, or inside X.
            bool toT = false, toF = false, bad = false;
            for (int m = from; m < k; ++m) {
                const Cls cl = classify(frags[m], ex, s, frags, from, k);
                if (cl == Cls::T) toT = true;
                else if (cl == Cls::F) toF = true;
                else if (cl == Cls::Bad) bad = true;
            }
            if (bad || (toT && toF) || (!toT && !toF)) continue;
            ExitSet sub;
            if (toF) { // X and Y
                sub.t = {s};
                sub.f = ex.f;
            } else { // X or Y
                sub.t = ex.t;
                sub.f = {s};
            }
            ExprP x = build(frags, from, k, nullptr, -1, sub);
            if (!x) continue;
            ExprP y = build(frags, k, to, leaf, leafStart, ex);
            if (!y) continue;
            ExprP e = MakeExpr(EK::Binary);
            e->op = toF ? BinOp::And : BinOp::Or;
            e->a = x;
            e->b = y;
            return e;
        }
        return nullptr;
    }

    // Decodes operand code from c.pc until a test that starts a fragment
    // (returns its pc), the end of the range (returns c.end) or failure (-1).
    int operandsUntilTest(Ctx& c, bool allowAlways = false) {
        while (c.pc < c.end) {
            if (c.leafDone) return c.pc == c.end ? c.end : -1;
            const Op o = GetOp(I(c.pc));
            // A constant operand of and/or compiles to a lone JMP.
            // `true or x` as a value starts with a lone JMP, which otherwise
            // is a constant operand of the condition being read.
            if (an_.isPlainJmp(c.pc) && an_.target(c.pc) > c.pc) {
                ValueCond vc;
                Snapshot s = snap(c);
                if (tryValueCond(c, vc)) continue;
                restore(c, s);
            }
            if (allowAlways && c.m.st.empty() && an_.isPlainJmp(c.pc) && an_.target(c.pc) > c.pc) return c.pc;
            if (an_.isTestAt(c.pc)) {
                ValueCond vc;
                Snapshot s = snap(c);
                if (tryValueCond(c, vc)) continue;
                restore(c, s);
                return c.pc;
            }
            if (o == JMP || o == RETURN || o == TAILCALL || o == FORLOOP || o == FORPREP || o == TFORLOOP ||
                o == CLOSE || an_.pseudo(c.pc))
                return -1;
            if (c.pc != c.loopStartOk && !an_.loopsAt(c.pc).empty()) return -1;
            if (!exprInstr(c)) return -1;
        }
        return c.pc == c.end ? c.end : -1;
    }

    struct ValueCond {
        bool allowDead = false; // accept a result nobody reads
    };

    // `a and b`, `a or b`, comparisons as values: the jumps all land inside a
    // short region that leaves one value in a register.
    // The region of a value condition starting with the test at pc: its end
    // (`final`), the result register and whether it ends in LOADBOOLs.
    struct ValueRegion {
        int final = -1, reg = -1, pF = -1, pT = -1;
        bool boolForm = false;
    };
    bool scanValueRegion(int pc, int end, ValueRegion& r) const {
        // A constant first operand (`true or x`, `false and x`) is a lone JMP.
        const bool lone = an_.isPlainJmp(pc);
        const int jmp = lone ? pc : pc + 1;
        int maxT = an_.target(jmp);
        if (maxT <= jmp) return false;
        int q = jmp + 1;
        while (q < maxT) {
            if (q >= end || q >= an_.size()) return false;
            const Op o = GetOp(I(q));
            if (an_.pseudo(q)) {
                ++q;
                continue;
            }
            if (o == JMP) {
                const int t = an_.target(q);
                if (t <= q) return false;
                maxT = std::max(maxT, t);
            } else if (o == LOADBOOL && GetC(I(q))) {
                maxT = std::max(maxT, q + 2);
            } else if (o == RETURN || o == TAILCALL || o == FORLOOP || o == FORPREP || o == TFORLOOP ||
                       o == CLOSE || o == SETGLOBAL || o == SETUPVAL || (o == CALL && GetC(I(q)) == 1)) {
                return false;
            }
            ++q;
        }
        const int final = q;
        if (final > end) return false;
        // Nothing outside may jump into the region.
        for (int x = jmp + 1; x < final; ++x)
            for (int src : an_.jumpsTo(x))
                if (src < pc || src >= final) return false;
        r.final = final;
        r.boolForm = final - 2 > jmp && GetOp(I(final - 1)) == LOADBOOL && GetOp(I(final - 2)) == LOADBOOL &&
                     GetA(I(final - 1)) == GetA(I(final - 2)) && GetB(I(final - 2)) == 0 &&
                     GetC(I(final - 2)) == 1 && GetB(I(final - 1)) == 1 && GetC(I(final - 1)) == 0;
        if (r.boolForm) {
            r.reg = GetA(I(final - 1));
            r.pF = final - 2;
            r.pT = final - 1;
        }
        // Tests that land on `final` leave their value in the result register:
        // TESTSET copies it there, TEST finds it there already (patchtestreg).
        for (int x = pc; x + 1 < final; ++x) {
            if (!an_.isTestAt(x) || an_.pseudo(x)) continue;
            if (an_.target(x + 1) != final) continue;
            if (GetOp(I(x)) != TEST && GetOp(I(x)) != TESTSET) return false;
            const int a = GetA(I(x));
            if (r.reg < 0) r.reg = a;
            else if (r.reg != a) return false;
        }
        return r.reg >= 0;
    }

    bool tryValueCond(Ctx& c, ValueCond& vc) {
        const int pc = c.pc;
        ValueRegion vr;
        if (!scanValueRegion(pc, c.end, vr)) return false;
        const int final = vr.final, reg = vr.reg, pF = vr.pF, pT = vr.pT;
        const bool boolForm = vr.boolForm;
        if (reg < 0) return false;
        const Snapshot s0 = snap(c);
        // The first fragment's operands are already on the stack.
        Frag f0;
        bool regIsLocal = reg < c.m.base;
        const bool lone = an_.isPlainJmp(pc);
        if (lone && !boolForm) return false;
        bool fragOk = true;
        if (lone) {
            f0.kind = Frag::Always;
            f0.pc = f0.jmp = pc;
            f0.target = an_.target(pc);
        } else {
            fragOk = makeFrag(c, pc, f0, regIsLocal ? -1 : reg, {}, regIsLocal ? -1 : reg);
        }
        if (!fragOk && !regIsLocal && c.out) {
            // The result register may be a pending local: `x = x and y`.
            restore(c, s0);
            fragOk = makeFrag(c, pc, f0, -1, {reg}) && reg < c.m.base;
            regIsLocal = true;
        }
        if (!fragOk) {
            restore(c, s0);
            return false;
        }
        if (regIsLocal) {
            // What's left pending are locals declared before this assignment.
            if (!c.out || !canPromoteBelow(c, c.m.top())) {
                restore(c, s0);
                return false;
            }
        } else if (reg != c.m.top()) {
            if (reg < c.m.base) noteScope(pc, reg);
            else if (reg < c.m.top()) noteScopePending(c, pc, reg);
            restore(c, s0);
            return false;
        }
        // Values still pending below the result that the condition reads are
        // locals declared before it.
        {
            int maxRead = -1;
            for (int x = pc + 1; x < final; ++x) {
                const RegSet r = an_.reads(x);
                for (int reg2 = c.m.base; reg2 < c.m.top() && reg2 < 256; ++reg2)
                    if (r[reg2]) maxRead = std::max(maxRead, reg2);
            }
            if (maxRead >= 0) {
                if (!c.out || !canPromoteBelow(c, maxRead + 1)) {
                    restore(c, s0);
                    return false;
                }
                promote(c, maxRead + 1);
            }
        }
        std::vector<Frag> frags{f0};
        const int tempBase = regIsLocal ? c.m.top() : reg;
        const int leafEnd = boolForm ? pF : final;
        int p = f0.jmp + 1;
        ExprP leaf;
        int leafStart = -1;
        bool vjmp = false;
        while (true) {
            if (boolForm && p == pF) {
                // The last fragment was a comparison falling into LOADBOOL false.
                vjmp = true;
                break;
            }
            int limit = leafEnd;
            // With LOADBOOLs and a value leaf, a JMP over them ends the leaf.
            if (boolForm && pF - 1 >= p && an_.isPlainJmp(pF - 1) && an_.target(pF - 1) == final) limit = pF - 1;
            Ctx oc = operandCtx(p, limit, tempBase, c.env);
            if (regIsLocal) oc.leafReg = reg;
            const int stop = operandsUntilTest(oc, true);
            if (stop < 0) {
                restore(c, s0);
                return false;
            }
            if (stop == limit) {
                // the trailing value
                leafStart = p;
                if (regIsLocal) {
                    if (p == limit && !oc.leafDone && oc.m.st.empty()) {
                        // `x = y or x`: the value is already in place, and
                        // jumps to final are exits rather than "go on".
                        leaf = localExpr(reg);
                        leafStart = -2;
                    } else if (!oc.leafDone || !oc.m.st.empty()) {
                        restore(c, s0);
                        return false;
                    } else {
                        leaf = oc.leafValue;
                    }
                } else {
                    if (oc.m.st.size() != 1 || oc.m.st[0].reg != reg || oc.m.st[0].selfFn || oc.m.st[0].table) {
                        if (!(oc.m.st.size() == 1 && oc.m.st[0].table && oc.m.st[0].reg == reg)) {
                            restore(c, s0);
                            return false;
                        }
                    }
                    leaf = oc.m.st[0].e;
                }
                if (limit != leafEnd && limit != pF - 1) {
                    restore(c, s0);
                    return false;
                }
                break;
            }
            Frag fk;
            if (!fragAt(oc, stop, fk, regIsLocal ? -1 : reg) || !oc.m.st.empty() || oc.leafDone) {
                restore(c, s0);
                return false;
            }
            fk.start = p;
            frags.push_back(fk);
            p = fk.jmp + 1;
        }
        // Constant operands (lone JMPs) only occur with the LOADBOOL ending.
        if (!boolForm)
            for (const Frag& fr : frags)
                if (fr.kind == Frag::Always) {
                    restore(c, s0);
                    return false;
                }
        // TESTs inside (not landing on final) behave as plain tests: A == B.
        for (const Frag& fr : frags) {
            if (fr.kind != Frag::Test) continue;
            if (fr.target == final) {
                if (fr.testA != reg) {
                    restore(c, s0);
                    return false;
                }
            } else if (fr.testA != fr.testB) {
                restore(c, s0);
                return false;
            }
        }
        ExitSet ex;
        if (boolForm) {
            ex.t = {pT, final};
            ex.f = {pF, final};
        } else {
            ex.t = {final};
            ex.f = {final};
        }
        ExprP e;
        if (vjmp) {
            e = build(frags, 0, int(frags.size()), nullptr, -1, ex);
        } else {
            e = build(frags, 0, int(frags.size()), leaf, leafStart, ex);
        }
        if (!e) {
            restore(c, s0);
            return false;
        }
        // A result nobody reads is more likely the end of an `if` block.
        if (!regIsLocal && !boolForm && !vc.allowDead && !an_.liveIn(final, reg) && c.out) {
            restore(c, s0);
            return false;
        }
        c.pc = final;
        if (regIsLocal) {
            noteLocalWrite(reg);
            auto st = MakeStmt(SK::Assign);
            st->lhs.push_back(localExpr(reg));
            st->rhs.push_back(e);
            if (!finishStore(c, st, final - 1)) {
                restore(c, s0);
                return false;
            }
            return true;
        }
        push(c, reg, e, final - 1);
        return true;
    }

    // Whether the next instruction puts a new value above the pending ones,
    // leaving a hole: the registers below it still hold locals.
    bool holeAhead(const Ctx& c) const {
        if (c.pc >= c.end) return false;
        const uint32_t i = I(c.pc);
        switch (GetOp(i)) {
            case MOVE: case LOADK: case LOADNIL: case GETUPVAL: case GETGLOBAL: case GETTABLE: case NEWTABLE:
            case SELF: case ADD: case SUB: case MUL: case DIV: case MOD: case POW: case UNM: case NOT: case LEN:
            case CONCAT: case CLOSURE: case VARARG:
                return GetA(i) > c.m.top();
            case LOADBOOL: return GetC(i) == 0 && GetA(i) > c.m.top();
            case EQ: case LT: case LE: case TEST: case TESTSET: {
                ValueRegion vr;
                return an_.isTestAt(c.pc) && scanValueRegion(c.pc, c.end, vr) && vr.reg > c.m.top();
            }
            case RETURN: return (GetB(i) == 0 || GetB(i) > 2) && GetA(i) > c.m.top();
            default: return false;
        }
    }

    bool conditional(Ctx& c) {
        const Snapshot s = snap(c);
        ValueCond vc;
        if (tryValueCond(c, vc)) return true;
        restore(c, s);
        if (!c.out) return false;
        // Values pending below the test's operands make an `if` unlikely: they
        // would all have to be locals. Read it as an unused value first.
        {
            const uint32_t i = I(c.pc);
            int operands = 0;
            if (GetOp(i) == TEST) operands = GetA(i) >= c.m.base ? 1 : 0;
            else if (GetOp(i) == TESTSET) operands = GetB(i) >= c.m.base ? 1 : 0;
            else operands = (!IsK(GetB(i)) && GetB(i) >= c.m.base) + (!IsK(GetC(i)) && GetC(i) >= c.m.base);
            if (int(c.m.st.size()) > operands) {
                vc.allowDead = true;
                if (tryValueCond(c, vc)) return true;
                restore(c, s);
                vc.allowDead = false;
            }
        }
        const bool ifOk = decodeIf(c);
        if (ifOk && !holeAhead(c)) return true;
        const Snapshot afterIf = snap(c);
        const size_t ifStmts = c.out->stmts.size();
        std::vector<StmtP> ifOut(c.out->stmts.begin() + std::min(s.stmts, ifStmts), c.out->stmts.end());
        restore(c, s);
        vc.allowDead = true;
        if (tryValueCond(c, vc)) return true;
        restore(c, s);
        if (ifOk) {
            // Keep the `if` reading after all.
            restore(c, afterIf);
            c.out->stmts.resize(s.stmts);
            c.out->stmts.insert(c.out->stmts.end(), ifOut.begin(), ifOut.end());
            return true;
        }
        return false;
    }

    // Collects fragments starting at c.pc whose operand code holds no statements.
    // In a statement's condition every TEST has A == B (lcode.c patchtestreg
    // with NO_REG); A != B copies a value, so it belongs to an expression.
    static bool PlainTest(const Frag& f) { return f.kind != Frag::Test || f.testA == f.testB; }

    bool collectRun(Ctx& c, std::vector<Frag>& run) {
        Frag f0;
        if (!makeFrag(c, c.pc, f0) || !PlainTest(f0)) return false;
        flush(c);
        if (!c.m.st.empty()) return false;
        f0.start = -1;
        run.push_back(f0);
        int q = f0.jmp + 1;
        while (q < c.end) {
            Ctx oc = operandCtx(q, c.end, nact_, c.env);
            const int stop = operandsUntilTest(oc);
            if (stop < 0 || stop >= c.end) break;
            Frag fk;
            if (!makeFrag(oc, stop, fk) || !oc.m.st.empty() || !PlainTest(fk)) break;
            fk.start = q;
            run.push_back(fk);
            q = fk.jmp + 1;
        }
        return true;
    }

    bool decodeIf(Ctx& c) {
        const Snapshot s0 = snap(c);
        std::vector<Frag> run;
        if (!collectRun(c, run)) {
            restore(c, s0);
            return false;
        }
        const Snapshot s1 = snap(c);
        for (int k = int(run.size()); k >= 1; --k) {
            const Frag& last = run[k - 1];
            const int P = last.jmp + 1;
            const int fact = last.target; // may lie behind when threaded into `until false`
            ExitSet ex;
            ex.t = an_.chain(P);
            ex.f = {fact};
            std::vector<Frag> frags(run.begin(), run.begin() + k);
            frags.back().forced = 1; // the condition's last test jumps when it is false
            ExprP cond = build(frags, 0, k, nullptr, -1, ex);
            if (!cond) continue;
            auto st = MakeStmt(SK::If);
            int next = -1;
            if (ifBody(c, cond, P, fact, *st, next)) {
                c.out->stmts.push_back(st);
                c.pc = next;
                return true;
            }
            restore(c, s1);
        }
        restore(c, s0);
        return false;
    }

    // The then block from P, an optional else, given the false exit `fact`.
    bool ifBody(Ctx& c, ExprP cond, int P, int fact, Stmt& st, int& next) {
        const int savedNact = nact_;
        const auto savedRegVar = regVar_;
        Block thenB;
        Ctx tc = blockCtx(P, c.end, &thenB, c.env);
        while (true) {
            const int q = tc.pc;
            if (q == fact && !an_.absorbs(q)) {
                closeBlock(tc);
                if (!tc.m.st.empty()) break;
                st.conds.push_back(cond);
                st.blocks.push_back(std::move(thenB));
                nact_ = savedNact;
                regVar_ = savedRegVar;
                next = fact;
                return true;
            }
            if (an_.isPlainJmp(q)) {
                const int E = an_.target(q);
                if (E > q && !an_.genForAtPrep(q) && an_.loopsAt(q).empty() && (q + 1 == fact || (an_.isPlainJmp(q + 1) && an_.inChain(q + 1, fact)))) {
                    const Snapshot ts = snap(tc);
                    const auto tRegVar = regVar_;
                    const int tNact = nact_;
                    closeBlock(tc);
                    if (tc.m.st.empty()) {
                        nact_ = savedNact;
                        Block elseB;
                        int enext = -1;
                        if (elseBody(c, q + 1, E, elseB, enext)) {
                            st.conds.push_back(cond);
                            st.blocks.push_back(std::move(thenB));
                            st.blocks.push_back(std::move(elseB));
                            st.hasElse = true;
                            nact_ = savedNact;
                            regVar_ = savedRegVar;
                            next = enext;
                            return true;
                        }
                    }
                    nact_ = tNact;
                    regVar_ = tRegVar;
                    restore(tc, ts);
                }
                if (q != fact && an_.inChain(q, fact)) {
                    closeBlock(tc);
                    if (tc.m.st.empty()) {
                        st.conds.push_back(cond);
                        st.blocks.push_back(std::move(thenB));
                        nact_ = savedNact;
                        regVar_ = savedRegVar;
                        next = q;
                        return true;
                    }
                }
            }
            if (q >= c.end) break;
            if (!step(tc)) {
                if (Trace()) std::fprintf(stderr, "ifBody from %d (false exit %d) failed at %d\n", P, fact, tc.pc);
                break;
            }
        }
        nact_ = savedNact;
        regVar_ = savedRegVar;
        return false;
    }

    bool elseBody(Ctx& c, int start, int E, Block& out, int& next) {
        const int savedNact = nact_;
        const auto savedRegVar = regVar_;
        Ctx ec = blockCtx(start, c.end, &out, c.env);
        while (true) {
            const int q = ec.pc;
            if ((q == E && !an_.absorbs(q)) || (q > start && q != E && an_.isPlainJmp(q) && an_.inChain(q, E))) {
                closeBlock(ec);
                if (!ec.m.st.empty()) break;
                nact_ = savedNact;
                regVar_ = savedRegVar;
                next = q;
                return true;
            }
            if (q >= c.end) break;
            if (!step(ec)) break;
        }
        nact_ = savedNact;
        regVar_ = savedRegVar;
        return false;
    }

    // All of [start, end) is a condition: fragments only.
    bool condRegion(int start, int end, const Env& env, std::vector<Frag>& frags, int loopStartOk = -1) {
        int q = start;
        while (q < end) {
            if (an_.isPlainJmp(q) && q == end - 1 && q == start) {
                Frag fr;
                fr.kind = Frag::Always;
                fr.pc = q;
                fr.jmp = q;
                fr.start = q;
                fr.target = an_.target(q);
                frags.push_back(fr);
                return true;
            }
            Ctx oc = operandCtx(q, end, nact_, env);
            oc.loopStartOk = loopStartOk;
            const int stop = operandsUntilTest(oc, true);
            if (stop < 0 || stop >= end) return false;
            Frag fk;
            if (!fragAt(oc, stop, fk) || !oc.m.st.empty() || !PlainTest(fk)) return false;
            fk.start = q;
            frags.push_back(fk);
            q = fk.jmp + 1;
        }
        return q == end && !frags.empty();
    }

    bool decodeLoop(Ctx& c, const Loop& l) {
        return l.kind == Loop::While ? decodeWhile(c, l) : decodeRepeat(c, l);
    }

    // A loop's body, read as statements; if other loops start where it does,
    // read again trying those first.
    bool loopBody(const Ctx& c, const Loop& l, Block& body) {
        Env env = c.env;
        env.loop = &l;
        env.loopAt = l.start;
        env.loopExit = l.exit;
        env.innerLoops = false;
        if (decodeRange(l.bodyStart, l.bodyEnd, env, body)) return true;
        if (l.bodyStart != l.start || an_.loopsAt(l.start).size() < 2) return false;
        body.stmts.clear();
        env.innerLoops = true;
        return decodeRange(l.bodyStart, l.bodyEnd, env, body);
    }

    // `while`: the condition, then the body, then a JMP back to the
    // condition. The condition's false exits leave the loop; without a
    // condition it is `while true do` (also what `repeat ... until false`
    // compiles to).
    bool decodeWhile(Ctx& c, const Loop& l0) {
        dropBefore(c, l0.start);
        flush(c);
        if (!c.m.st.empty()) return false;
        if (l0.exit > c.end) return false;
        std::vector<Frag> run;
        for (int q = l0.start; q < l0.bodyEnd;) {
            Ctx oc = operandCtx(q, l0.bodyEnd, nact_, c.env);
            oc.loopStartOk = l0.start;
            const int stop = operandsUntilTest(oc, true);
            if (stop < 0 || stop >= l0.bodyEnd) break;
            Frag fk;
            if (!fragAt(oc, stop, fk) || !oc.m.st.empty() || !PlainTest(fk)) break;
            fk.start = q;
            run.push_back(fk);
            q = fk.jmp + 1;
        }
        const std::vector<int> exits = an_.chain(l0.exit);
        for (int k = int(run.size()); k >= 0; --k) {
            Loop l = l0;
            ExprP cond;
            if (k == 0) {
                cond = MakeExpr(EK::True);
                l.bodyStart = l0.start;
            } else {
                const Frag& last = run[size_t(k - 1)];
                if (std::find(exits.begin(), exits.end(), last.target) == exits.end()) continue;
                l.bodyStart = last.jmp + 1;
                ExitSet ex;
                ex.t = an_.chain(l.bodyStart);
                ex.f = exits;
                std::vector<Frag> frags(run.begin(), run.begin() + k);
                frags.back().forced = 1;
                cond = build(frags, 0, k, nullptr, -1, ex);
                if (!cond) continue;
            }
            auto st = MakeStmt(SK::While);
            st->e = cond;
            if (loopBody(c, l, st->body)) {
                c.out->stmts.push_back(st);
                c.pc = l.exit;
                return true;
            }
        }
        return false;
    }

    // `repeat`: the body, then the `until` condition, whose false exits jump
    // back to the start. The condition still sees the body's locals.
    bool decodeRepeat(Ctx& c, const Loop& l) {
        dropBefore(c, l.start);
        flush(c);
        if (!c.m.st.empty()) return false;
        if (l.exit > c.end) return false;
        if (repeatOnce(c, l, false)) return true;
        return an_.loopsAt(l.start).size() >= 2 && repeatOnce(c, l, true);
    }

    bool repeatOnce(Ctx& c, const Loop& l, bool innerLoops) {
        const int savedNact = nact_;
        const auto savedRegVar = regVar_;
        auto st = MakeStmt(SK::Repeat);
        Env env = c.env;
        env.loop = &l;
        env.loopAt = l.start;
        env.loopExit = l.exit;
        env.innerLoops = innerLoops;
        Ctx bc = blockCtx(l.start, l.exit, &st->body, env);
        while (true) {
            const int q = bc.pc;
            {
                const Snapshot bs = snap(bc);
                const auto bRegVar = regVar_;
                const int bNact = nact_;
                closeBlock(bc);
                if (bc.m.st.empty()) {
                    std::vector<Frag> frags;
                    if (condRegion(q, l.exit, bc.env, frags, l.start) && an_.target(frags.back().jmp) == l.start) {
                        ExitSet ex;
                        ex.t = an_.chain(l.exit);
                        ex.f = {l.start};
                        if (ExprP cond = build(frags, 0, int(frags.size()), nullptr, -1, ex)) {
                            st->e = cond;
                            nact_ = savedNact;
                            regVar_ = savedRegVar;
                            c.out->stmts.push_back(st);
                            c.pc = l.exit;
                            return true;
                        }
                    }
                }
                nact_ = bNact;
                regVar_ = bRegVar;
                restore(bc, bs);
            }
            if (q >= l.exit) break;
            if (!step(bc)) break;
        }
        nact_ = savedNact;
        regVar_ = savedRegVar;
        return false;
    }

    // Numeric `for`: init, limit and step in three registers, FORPREP to the
    // FORLOOP at the end of the body, which jumps back while it runs. The
    // loop variable is the register after them.
    bool decodeNumFor(Ctx& c) {
        const int pc = c.pc;
        const Loop* l = an_.numForAtPrep(pc);
        if (!l || !c.out) return false;
        const int A = GetA(I(pc));
        if (l->exit > c.end) return false;
        // init, limit and step are the newest three values.
        if (A < c.m.base) noteScope(pc, A);
        if (c.m.top() != A + 3 || A < c.m.base) return false;
        for (int r = A; r < A + 3; ++r) {
            const Slot* s = c.m.at(r);
            if (!s || s->selfFn || s->selfObj || s->group >= 0 || IsOpen(s->e)) return false;
        }
        if (!canPromoteBelow(c, A)) return false;
        promote(c, A);
        std::vector<ExprP> vals;
        for (const Slot& s : c.m.st) vals.push_back(s.e);
        c.m.st.clear();
        auto st = MakeStmt(SK::NumFor);
        st->rhs = vals;
        // The default step is LOADK 1, which `, 1` compiles to as well.
        const ExprP& step = vals[2];
        if (step->k == EK::Number && step->num == 1.0f && step->kidx >= 0) {
            st->stepK = step->kidx;
            st->rhs.pop_back();
        }
        Var* v = d_.newVar(A + 3);
        v->hint = "i";
        st->vars.push_back(v);
        const int savedNact = nact_;
        const auto savedRegVar = regVar_;
        regVar_[A] = d_.newVar(A);
        regVar_[A + 1] = d_.newVar(A + 1);
        regVar_[A + 2] = d_.newVar(A + 2);
        regVar_[A + 3] = v;
        nact_ = A + 4;
        Env env = c.env;
        env.loop = l;
        const bool ok = decodeRange(l->bodyStart, l->bodyEnd, env, st->body);
        nact_ = savedNact;
        regVar_ = savedRegVar;
        if (!ok) return false;
        c.out->stmts.push_back(st);
        c.pc = l->exit;
        c.m.base = nact_;
        return true;
    }

    // Generic `for`: the explist adjusted to three values (generator, state,
    // control), a JMP to the TFORLOOP after the body, which calls the
    // generator into the variables after them and jumps back while the first
    // isn't nil.
    bool decodeGenFor(Ctx& c) {
        const int pc = c.pc;
        const Loop* l = an_.genForAtPrep(pc);
        if (!l || !c.out) return false;
        if (l->exit > c.end) return false;
        const uint32_t tfor = I(l->bodyEnd);
        const int A = GetA(tfor);
        const int nvars = GetC(tfor);
        if (nvars < 1) return false;
        if (A < c.m.base) noteScope(pc, A);
        if (c.m.top() != A + 3 || A < c.m.base) return false;
        if (!canPromoteBelow(c, A)) return false;
        promote(c, A);
        auto st = MakeStmt(SK::GenFor);
        // The explist was adjusted to three values: drop nil padding and the
        // extra results of a trailing call.
        int lastGroup = -2;
        int lastReal = -1;
        for (int j = 0; j < int(c.m.st.size()); ++j)
            if (!c.m.st[j].fromLoadNil) lastReal = j;
        for (int j = 0; j < int(c.m.st.size()); ++j) {
            const Slot& s = c.m.st[j];
            if (s.selfFn || s.selfObj) return false;
            if (s.group >= 0) {
                if (s.group == lastGroup) continue;
                lastGroup = s.group;
            }
            if (s.fromLoadNil && j > lastReal && j > 0) continue;
            st->rhs.push_back(s.e);
        }
        c.m.st.clear();
        const int savedNact = nact_;
        const auto savedRegVar = regVar_;
        regVar_[A] = d_.newVar(A);
        regVar_[A + 1] = d_.newVar(A + 1);
        regVar_[A + 2] = d_.newVar(A + 2);
        for (int j = 0; j < nvars; ++j) {
            Var* v = d_.newVar(A + 3 + j);
            v->hint = nvars == 2 ? (j == 0 ? "k" : "v") : nvars == 1 ? "v" : "v" + std::to_string(j + 1);
            st->vars.push_back(v);
            regVar_[A + 3 + j] = v;
        }
        nact_ = A + 3 + nvars;
        Env env = c.env;
        env.loop = l;
        const bool ok = decodeRange(l->bodyStart, l->bodyEnd, env, st->body);
        nact_ = savedNact;
        regVar_ = savedRegVar;
        if (!ok) return false;
        c.out->stmts.push_back(st);
        c.pc = l->exit;
        c.m.base = nact_;
        return true;
    }

    // Name hint for a local from its initial value.
    static std::string Hint(const ExprP& e) {
        if (!e) return {};
        auto fromName = [](std::string s) -> std::string {
            if (s.size() > 3 && s.compare(0, 3, "Get") == 0 && std::isupper(static_cast<unsigned char>(s[3])))
                s = s.substr(3);
            // lowerCamel: leading capitals lowered (`AIRacer` -> `aiRacer`).
            size_t n = 0;
            while (n < s.size() && std::isupper(static_cast<unsigned char>(s[n]))) ++n;
            if (n > 1 && n < s.size()) --n;
            for (size_t j = 0; j < n; ++j) s[j] = char(std::tolower(static_cast<unsigned char>(s[j])));
            return IsIdentifier(s) ? s : std::string();
        };
        auto prefixed = [](const char* pre, const std::string& h) -> std::string {
            if (h.empty()) return pre == std::string("is") ? "flag" : std::string();
            std::string s = h;
            s[0] = char(std::toupper(static_cast<unsigned char>(s[0])));
            return pre + s;
        };
        switch (e->k) {
            case EK::Index:
                if (e->b && e->b->k == EK::String) return fromName(e->b->str);
                return Hint(e->a);
            case EK::Global: return fromName(e->str);
            case EK::Call:
                if (e->a && e->a->k == EK::Index && e->a->b && e->a->b->k == EK::String) return fromName(e->a->b->str);
                if (e->a && e->a->k == EK::Global) return fromName(e->a->str);
                return {};
            case EK::Table: return "t";
            case EK::Function: return "fn";
            case EK::Number: return "n";
            case EK::String: return "s";
            case EK::True: case EK::False: return "flag";
            case EK::Unary:
                if (e->uop == UnOp::Not) return prefixed("not", Hint(e->a));
                if (e->uop == UnOp::Len) return "count";
                return Hint(e->a);
            case EK::Binary:
                switch (e->op) {
                    case BinOp::Eq: case BinOp::Ne: case BinOp::Lt: case BinOp::Le: {
                        // Name it after the side that isn't a literal or local.
                        const ExprP& side = (e->b->k == EK::Index || e->b->k == EK::Call || e->b->k == EK::Global) &&
                                                    !(e->a->k == EK::Index || e->a->k == EK::Call || e->a->k == EK::Global)
                                                ? e->b
                                                : e->a;
                        return prefixed("is", Hint(side));
                    }
                    case BinOp::And: return Hint(e->b);
                    case BinOp::Or: return Hint(e->a);
                    case BinOp::Concat: return "text";
                    default: return Hint(e->a);
                }
            default: return {};
        }
    }

    Decomp& d_;
    const Function& f_;
    Analysis an_;
    FuncAst& ast_;
    std::vector<Var*> regVar_;
    std::vector<int> lastConsumed_;
    std::vector<std::vector<int>> localWrites_; // register -> statements assigning it as a local
    int scopeReg_ = -1;
    std::vector<int> scopeCands_;               // where the noted scope may end, oldest first
    std::map<int, std::set<int>> scopeTried_;   // register -> scope ends tried
    int lastScopeReg_ = -1;
    std::vector<int> lastScopeCands_;
    std::set<std::pair<int, int>> scopeEnds_;   // (pc, register): locals from there end before pc
    int scopePc_ = -1;
    std::pair<int, int> scopeHint_{-1, -1};
    int stmtStart_ = 0;                         // where the statement being decoded began
    std::set<std::pair<int, int>> forced_; // (pc, register): read as a local there
    int gapPc_ = -1;
    std::vector<std::pair<int, int>> gapHints_;
    int nact_ = 0;
    int groupId_ = 0;
    int elidedHint_ = 0; // registers below this are locals from the function's start
};

// ============================================================================
// Naming
// ============================================================================

// Names are unique among the variables visible at a point (and never a
// global the chunk uses), so no name shadows another the code refers to.
class Namer {
public:
    explicit Namer(const Decomp& d) : globals_(d.globals) {}

    void func(FuncAst& f, bool handler) {
        static const char* const kHandlerParams[] = {"self", "message", "vars"};
        scopes_.emplace_back();
        for (size_t i = 0; i < f.params.size(); ++i) {
            Var* v = f.params[i];
            if (handler && i < 3 && v->name.empty()) v->name = kHandlerParams[i];
            if (!v->name.empty()) declare(v->name);
            else assign(v, f.params.size() == 1 ? "p" : "p" + std::to_string(i + 1));
        }
        if (f.argVar) declare("arg");
        block(f.body);
        scopes_.pop_back();
    }

private:
    bool taken(const std::string& n) const {
        if (IsReserved(n) || globals_.count(n)) return true;
        for (const auto& sc : scopes_)
            if (sc.count(n)) return true;
        return false;
    }
    void declare(const std::string& n) { scopes_.back().insert(n); }

    void assign(Var* v, std::string hint) {
        if (!v || !v->name.empty()) return;
        if (hint.empty() || !IsIdentifier(hint)) hint = "local";
        const bool numbered = hint == "local" || hint == "fn";
        std::string name = numbered ? hint + "1" : hint;
        // A name ending in a digit gets a separator before its counter.
        const std::string stem = std::isdigit(static_cast<unsigned char>(hint.back())) ? hint + "_" : hint;
        for (int n = 2; taken(name); ++n) name = stem + std::to_string(n);
        v->name = name;
        declare(name);
    }

    void expr(const ExprP& e) {
        if (!e) return;
        if (e->k == EK::Function && e->fn) func(*e->fn, e->fn->handler);
        expr(e->a);
        expr(e->b);
        for (const auto& x : e->args) expr(x);
        for (const auto& it : e->items) {
            expr(it.key);
            expr(it.value);
        }
    }

    void block(Block& b) {
        scopes_.emplace_back();
        for (auto& s : b.stmts) stmt(*s);
        scopes_.pop_back();
    }

    void stmt(Stmt& s) {
        for (auto& e : s.lhs) expr(e);
        for (auto& e : s.rhs) expr(e);
        switch (s.k) {
            case SK::Local:
                for (Var* v : s.vars) assign(v, v->hint);
                return;
            case SK::LocalFunction:
                for (Var* v : s.vars) assign(v, v->hint.empty() ? "fn" : v->hint);
                expr(s.e);
                return;
            case SK::NumFor: case SK::GenFor:
                scopes_.emplace_back();
                for (Var* v : s.vars) assign(v, v->hint);
                block(s.body);
                scopes_.pop_back();
                return;
            case SK::Repeat:
                block(s.body);
                expr(s.e);
                return;
            case SK::If:
                for (size_t j = 0; j < s.blocks.size(); ++j) {
                    if (j < s.conds.size()) expr(s.conds[j]);
                    block(s.blocks[j]);
                }
                return;
            default:
                expr(s.e);
                block(s.body);
                return;
        }
    }

    std::set<std::string> globals_;
    std::vector<std::set<std::string>> scopes_;
};

// ============================================================================
// Printer
// ============================================================================

class Printer {
public:
    struct Line {
        int indent = 0;
        std::string text;
        int anchor = -1;     // required 1-based source line
        bool opener = false; // ends with then/do/else/repeat/a function header
    };

    explicit Printer(Decomp& d) : d_(d) {}

    void chunk(const FuncAst& main) {
        pushFunc(main);
        block(main.body);
        injectUnref();
        popFunc();
        flushLine();
    }

    std::string layout() {
        std::vector<Line> out;
        int lastAnchorIdx = -1; // index in out of the last line placed by an anchor
        for (const Line& l : lines_) {
            if (l.anchor >= 1) {
                const int want = l.anchor;
                while (int(out.size()) + 1 < want) out.push_back(Line{});
                // Lines after the last placed one may join it (a function's
                // body and `end` on its header's line).
                int excess = int(out.size()) + 1 - want;
                if (excess > 0) shrink(out, lastAnchorIdx, excess);
                if (int(out.size()) + 1 > want && !out.empty()) {
                    join(out.back(), l);
                    if (int(out.size()) != want) d_.warn("could not place a function on its original line");
                    lastAnchorIdx = int(out.size()) - 1;
                    continue;
                }
                lastAnchorIdx = int(out.size());
            }
            out.push_back(l);
        }
        // A statement starting with `(` would continue a call on the line
        // before ("ambiguous syntax"): end that statement with `;`.
        for (size_t j = 1; j < out.size(); ++j) {
            if (out[j].text.empty() || out[j].text[0] != '(') continue;
            size_t p = j;
            while (p > 0 && (out[p - 1].text.empty() || out[p - 1].text.rfind("--", 0) == 0)) --p;
            if (p == 0) continue;
            Line& prev = out[p - 1];
            if (!prev.opener && prev.text.back() != ';') prev.text += ";";
        }
        std::string s;
        for (const Line& l : out) {
            if (!l.text.empty()) {
                s.append(size_t(l.indent), '\t');
                s += l.text;
            }
            s += '\n';
        }
        return s;
    }

private:
    static bool Closing(const std::string& t) {
        return t.empty() || t.rfind("end", 0) == 0 || t.rfind("else", 0) == 0 || t.rfind("until", 0) == 0;
    }

    // Removes `count` line breaks from out[from..]: first blank lines, then
    // breaks next to block keywords (`if c then return end` on one line), then
    // the latest others.
    void shrink(std::vector<Line>& out, int from, int count) {
        if (Trace()) std::fprintf(stderr, "layout: joining %d lines\n", count);
        from = std::max(from, 0);
        // `if c then` / one statement / `end` becomes one line.
        for (int i = int(out.size()) - 1; i - 2 >= from && count >= 2; --i) {
            if (!out[i - 2].opener || out[i - 1].opener || Closing(out[i - 1].text) || out[i].text != "end") continue;
            const Line mid = out[i - 1], last = out[i];
            out.erase(out.begin() + (i - 1), out.begin() + i + 1);
            join(out[i - 2], mid);
            join(out[i - 2], last);
            count -= 2;
            i -= 2;
        }
        for (int pass = 0; pass < 3 && count > 0; ++pass) {
            for (int i = int(out.size()) - 1; i > from && count > 0; --i) {
                const Line& a = out[i - 1];
                const Line& b = out[i];
                bool pick;
                if (pass == 0) pick = b.text.empty();
                else if (pass == 1) pick = a.opener || Closing(b.text);
                else pick = true;
                if (!pick) continue;
                const Line next = out[i];
                out.erase(out.begin() + i);
                join(out[i - 1], next);
                --count;
            }
        }
    }

    static void join(Line& into, const Line& next) {
        if (next.text.empty()) return;
        if (into.text.empty()) {
            into.text = next.text;
            into.indent = next.indent;
            into.opener = next.opener;
            if (into.anchor < 0) into.anchor = next.anchor;
            return;
        }
        const bool needSemi = next.text[0] == '(' && !into.opener;
        into.text += needSemi ? "; " : " ";
        into.text += next.text;
        into.opener = next.opener;
    }

    // ---- line building ----
    void text(const std::string& s) { cur_ += s; }
    void anchor(int line) {
        if (curAnchor_ < 0) curAnchor_ = line;
    }
    void endLine(bool opener = false) {
        Line l;
        l.indent = indent_;
        l.text = cur_;
        l.anchor = curAnchor_;
        l.opener = opener;
        lines_.push_back(l);
        cur_.clear();
        curAnchor_ = -1;
    }
    void flushLine() {
        if (!cur_.empty()) endLine();
    }

    // ---- constant tracking (source order of first use) ----
    struct FuncState {
        const FuncAst* f;
        std::set<int> seen;
        std::vector<bool> unref;
    };
    void pushFunc(const FuncAst& f) { funcs_.push_back({&f, {}, UnreferencedConstants(*f.proto)}); }
    int nextNewK() const {
        int i = 0;
        while (funcs_.back().seen.count(i)) ++i;
        return i;
    }
    bool nextIsUnref() const {
        const int i = nextNewK();
        return i < int(funcs_.back().unref.size()) && funcs_.back().unref[size_t(i)];
    }
    bool nextUnrefString() const {
        return nextIsUnref() &&
               funcs_.back().f->proto->constants[size_t(nextNewK())].type == Constant::Type::String;
    }
    // Constants nothing refers to, as `if <constant> then end` (no code).
    void injectUnref() {
        while (nextIsUnref()) {
            const int i = nextNewK();
            text("if ");
            constantLiteral(i);
            text(" then end");
            endLine();
        }
    }
    void constantLiteral(int idx) {
        const Constant& k = funcs_.back().f->proto->constants[size_t(idx)];
        ExprP e;
        if (k.type == Constant::Type::Number) {
            e = MakeExpr(EK::Number);
            e->num = k.number;
        } else if (k.type == Constant::Type::Boolean) {
            e = MakeExpr(k.boolean ? EK::True : EK::False);
        } else if (k.type == Constant::Type::String) {
            e = MakeExpr(EK::String);
            e->str = k.string;
        } else {
            e = MakeExpr(EK::Nil);
        }
        e->kidx = idx;
        expr(e);
        markK(idx);
    }
    void popFunc() { funcs_.pop_back(); }
    void markK(int idx) {
        if (idx >= 0) funcs_.back().seen.insert(idx);
    }
    // New constants (not yet printed) of e, in the order printing adds them.
    void newKSeq(const ExprP& e, std::vector<int>& out) const {
        if (!e) return;
        auto add = [&](int idx) {
            if (idx >= 0 && !funcs_.back().seen.count(idx) && std::find(out.begin(), out.end(), idx) == out.end())
                out.push_back(idx);
        };
        switch (e->k) {
            case EK::Nil: case EK::True: case EK::False: case EK::Number: case EK::String: case EK::Global:
                add(e->kidx);
                return;
            case EK::Function: return;
            case EK::Call:
                if (e->method) {
                    newKSeq(e->a->a, out);
                    newKSeq(e->a->b, out);
                } else {
                    newKSeq(e->a, out);
                }
                for (const auto& x : e->args) newKSeq(x, out);
                return;
            case EK::Binary:
                if (e->op == BinOp::Concat && !e->args.empty()) {
                    for (const auto& x : e->args) newKSeq(x, out);
                    return;
                }
                if ((e->op == BinOp::Lt || e->op == BinOp::Le) && SwapCompare(e)) {
                    newKSeq(e->b, out);
                    newKSeq(e->a, out);
                    return;
                }
                // A numeral on the left of arithmetic waits for constant
                // folding: codearith adds the right operand's constant first.
                if (IsArith(e->op) && e->a->k == EK::Number) {
                    newKSeq(e->b, out);
                    newKSeq(e->a, out);
                    return;
                }
                break;
            case EK::Table:
                for (const auto& it : e->items) {
                    newKSeq(it.key, out);
                    newKSeq(it.value, out);
                }
                return;
            default: break;
        }
        newKSeq(e->a, out);
        newKSeq(e->b, out);
        for (const auto& x : e->args) newKSeq(x, out);
    }

    // `a > b` compiles to `b < a` with a evaluated first: two temporaries are
    // in evaluation order, and constants enter the constant table in source
    // order. When neither tells, keep literals on the right.
    bool SwapCompare(const ExprP& e) const {
        if (e->lreg >= 0 && e->rreg >= 0) return e->lreg > e->rreg;
        std::vector<int> natural, swapped;
        newKSeq(e->a, natural);
        newKSeq(e->b, natural);
        newKSeq(e->b, swapped);
        newKSeq(e->a, swapped);
        const bool natOk = std::is_sorted(natural.begin(), natural.end());
        const bool swapOk = std::is_sorted(swapped.begin(), swapped.end());
        if (natOk != swapOk) return swapOk;
        return IsLiteral(e->a) && !IsLiteral(e->b);
    }

    // ---- names ----
    std::string upvalName(int idx) const {
        const FuncAst* f = funcs_.back().f;
        if (idx >= 0 && idx < int(f->upvals.size()) && f->upvals[idx]) return f->upvals[idx]->name;
        return "upvalue" + std::to_string(idx);
    }

    // ---- expressions ----
    static bool IsArith(BinOp op) {
        return op == BinOp::Add || op == BinOp::Sub || op == BinOp::Mul || op == BinOp::Div || op == BinOp::Mod ||
               op == BinOp::Pow;
    }
    static int Prec(const ExprP& e) {
        switch (e->k) {
            case EK::Binary:
                switch (e->op) {
                    case BinOp::Or: return 1;
                    case BinOp::And: return 2;
                    case BinOp::Eq: case BinOp::Ne: case BinOp::Lt: case BinOp::Le: return 3;
                    case BinOp::Concat: return 5;
                    case BinOp::Add: case BinOp::Sub: return 6;
                    case BinOp::Mul: case BinOp::Div: case BinOp::Mod: return 7;
                    case BinOp::Pow: return 10;
                }
                return 0;
            case EK::Unary: return 8;
            case EK::Number: return std::signbit(e->num) ? 8 : 100;
            default: return 100;
        }
    }
    static bool IsLiteral(const ExprP& e) {
        return e->k == EK::Nil || e->k == EK::True || e->k == EK::False || e->k == EK::Number || e->k == EK::String;
    }
    static bool IsPrefixExp(const ExprP& e) {
        return e->k == EK::Local || e->k == EK::Upval || e->k == EK::Global || e->k == EK::Index ||
               e->k == EK::Call || e->k == EK::Paren;
    }

    void exprIn(const ExprP& e, int minPrec) {
        if (Prec(e) < minPrec) {
            text("(");
            expr(e);
            text(")");
        } else {
            expr(e);
        }
    }

    void prefix(const ExprP& e) {
        if (IsPrefixExp(e)) {
            expr(e);
        } else {
            text("(");
            expr(e);
            text(")");
        }
    }

    void args(const std::vector<ExprP>& list, bool multretEnd) {
        for (size_t j = 0; j < list.size(); ++j) {
            if (j) text(", ");
            const ExprP& a = list[j];
            // A string constant nothing refers to, read before this value's
            // own constants: `"x" and v` evaluates to v and leaves "x" behind.
            if (nextUnrefString() && !IsLiteral(a)) {
                std::vector<int> ks;
                newKSeq(a, ks);
                if (!ks.empty() && *std::min_element(ks.begin(), ks.end()) > nextNewK()) {
                    constantLiteral(nextNewK());
                    text(" and ");
                    if (Prec(a) <= 2) {
                        text("(");
                        expr(a);
                        text(")");
                        continue;
                    }
                }
            }
            // A single-result call at the end of a list that keeps all results.
            if (multretEnd && j + 1 == list.size() && IsMulti(a) && !a->open) {
                text("(");
                expr(a);
                text(")");
            } else {
                expr(a);
            }
        }
    }

    void expr(const ExprP& e) {
        switch (e->k) {
            case EK::Nil: markK(e->kidx); text("nil"); break;
            case EK::True: case EK::False:
                // `not "x"` folds to false and leaves "x" in the constant table.
                if (e->kidx < 0 && nextUnrefString()) {
                    text(e->k == EK::True ? "not not " : "not ");
                    constantLiteral(nextNewK());
                    break;
                }
                markK(e->kidx);
                text(e->k == EK::True ? "true" : "false");
                break;
            case EK::Number: {
                // `-<numeral>` folds into the negative constant.
                const bool neg = std::signbit(e->num) && !std::isnan(e->num);
                std::string s = FormatNumber(neg ? -e->num : e->num);
                if (s.empty()) {
                    d_.warn("a number constant has no Lua literal");
                    s = "0";
                }
                markK(e->kidx);
                text(neg ? "-" + s : s);
                break;
            }
            case EK::Vararg: text("..."); break;
            case EK::String: markK(e->kidx); text(QuoteString(e->str)); break;
            case EK::Local: text(e->var ? e->var->name : "?"); break;
            case EK::Upval: text(upvalName(e->up)); break;
            case EK::Global: markK(e->kidx); text(e->str); break;
            case EK::Index:
                prefix(e->a);
                if (e->b->k == EK::String && IsIdentifier(e->b->str)) {
                    markK(e->b->kidx);
                    text("." + e->b->str);
                } else {
                    text("[");
                    expr(e->b);
                    text("]");
                }
                break;
            case EK::Call:
                if (e->method) {
                    prefix(e->a->a);
                    markK(e->a->b->kidx);
                    text(":" + e->a->b->str);
                } else {
                    prefix(e->a);
                }
                text("(");
                args(e->args, true);
                text(")");
                break;
            case EK::Function: function(*e->fn, "function", nullptr); break;
            case EK::Table: table(e); break;
            case EK::Paren: text("("); expr(e->a); text(")"); break;
            case EK::Unary: {
                text(e->uop == UnOp::Not ? "not " : e->uop == UnOp::Len ? "#" : "-");
                // `- -x` would start a comment.
                const bool neg = e->uop == UnOp::Neg &&
                                 ((e->a->k == EK::Unary && e->a->uop == UnOp::Neg) ||
                                  (e->a->k == EK::Number && std::signbit(e->a->num)));
                if (neg) {
                    text("(");
                    expr(e->a);
                    text(")");
                } else {
                    exprIn(e->a, 8);
                }
                break;
            }
            case EK::Binary: binary(e); break;
        }
    }

    void binary(const ExprP& e) {
        if (e->op == BinOp::Concat && !e->args.empty()) {
            for (size_t j = 0; j < e->args.size(); ++j) {
                if (j) text(" .. ");
                // `..` is right associative: a concat operand needs parentheses.
                const ExprP& x = e->args[j];
                const bool paren = (x->k == EK::Binary && x->op == BinOp::Concat) || Prec(x) < 6;
                if (paren) {
                    text("(");
                    expr(x);
                    text(")");
                } else {
                    expr(x);
                }
            }
            return;
        }
        ExprP l = e->a, r = e->b;
        const char* op = "?";
        int lp = 0, rp = 0;
        switch (e->op) {
            case BinOp::Add: op = "+"; lp = 6; rp = 7; break;
            case BinOp::Sub: op = "-"; lp = 6; rp = 7; break;
            case BinOp::Mul: op = "*"; lp = 7; rp = 8; break;
            case BinOp::Div: op = "/"; lp = 7; rp = 8; break;
            case BinOp::Mod: op = "%"; lp = 7; rp = 8; break;
            case BinOp::Pow: op = "^"; lp = 11; rp = 10; break;
            case BinOp::Eq: op = "=="; lp = 4; rp = 4; break;
            case BinOp::Ne: op = "~="; lp = 4; rp = 4; break;
            case BinOp::Lt: case BinOp::Le: {
                // `a > b` compiles to `b < a` with a evaluated first: two
                // temporaries are in evaluation order, and constants enter the
                // constant table in source order. Otherwise either way round
                // compiles the same; keep literals on the right.
                const bool swap = SwapCompare(e);
                if (swap) {
                    std::swap(l, r);
                    op = e->op == BinOp::Lt ? ">" : ">=";
                } else {
                    op = e->op == BinOp::Lt ? "<" : "<=";
                }
                lp = 4;
                rp = 4;
                break;
            }
            case BinOp::And: op = "and"; lp = 2; rp = 2; break;
            case BinOp::Or: op = "or"; lp = 1; rp = 1; break;
            case BinOp::Concat: op = ".."; lp = 6; rp = 5; break;
        }
        // and/or chains compile the same either way round; keep them flat.
        if ((e->op == BinOp::And || e->op == BinOp::Or) && r->k == EK::Binary && r->op == e->op) rp = Prec(r);
        exprIn(l, lp);
        text(std::string(" ") + op + " ");
        exprIn(r, rp);
    }

    void table(const ExprP& e) {
        if (e->items.empty()) {
            text("{}");
            return;
        }
        text("{");
        size_t lastList = SIZE_MAX;
        for (size_t j = 0; j < e->items.size(); ++j)
            if (!e->items[j].key) lastList = j;
        for (size_t j = 0; j < e->items.size(); ++j) {
            if (j) text(", ");
            const TableItem& it = e->items[j];
            if (!it.key) {
                if (j == lastList && IsMulti(it.value) && !it.value->open) {
                    text("(");
                    expr(it.value);
                    text(")");
                } else {
                    expr(it.value);
                }
                continue;
            }
            if (it.key->k == EK::String && IsIdentifier(it.key->str)) {
                markK(it.key->kidx);
                text(it.key->str + " = ");
            } else {
                text("[");
                expr(it.key->k == EK::Paren ? it.key->a : it.key);
                text("] = ");
            }
            expr(it.value);
        }
        text("}");
    }

    // `function name(params)` (or `function(params)` when name is "function").
    void function(const FuncAst& f, const std::string& head, const std::function<void()>* printName) {
        text(head);
        if (printName) (*printName)();
        anchor(f.proto->lineDefined);
        text("(");
        for (size_t j = 0; j < f.params.size(); ++j) {
            if (j) text(", ");
            text(f.params[j]->name);
        }
        if (f.vararg) text(f.params.empty() ? "..." : ", ...");
        text(")");
        endLine(true);
        ++indent_;
        pushFunc(f);
        block(f.body);
        injectUnref();
        popFunc();
        flushLine();
        --indent_;
        text("end");
        anchor(f.proto->lastLineDefined); // the line of `end` is recorded too
    }

    // ---- statements ----
    void block(const Block& b) {
        for (size_t j = 0; j < b.stmts.size(); ++j) {
            const Stmt& s = *b.stmts[j];
            if (!(s.k == SK::While && s.e->k == EK::True)) injectUnref();
            // `return` and `break` must end a block.
            if ((s.k == SK::Return || s.k == SK::Break) && j + 1 < b.stmts.size()) {
                text("do ");
                stmtInline(s);
                text(" end");
                endLine();
                continue;
            }
            stmt(s);
        }
    }

    void stmtInline(const Stmt& s) {
        if (s.k == SK::Break) {
            text("break");
            return;
        }
        text("return");
        if (!s.rhs.empty()) {
            text(" ");
            args(s.rhs, true);
        }
    }

    void exprList(const std::vector<ExprP>& list) { args(list, false); }

    // Field path `a.b.c` printable as a function name.
    static bool FuncNamePath(const ExprP& e) {
        if (e->k == EK::Global || e->k == EK::Local || e->k == EK::Upval) return true;
        if (e->k == EK::Index && e->b->k == EK::String && IsIdentifier(e->b->str)) return FuncNamePath(e->a);
        return false;
    }

    void stmt(const Stmt& s) {
        if (!s.warning.empty()) d_.warn(s.warning);
        switch (s.k) {
            case SK::Local: {
                text("local ");
                for (size_t j = 0; j < s.vars.size(); ++j) {
                    if (j) text(", ");
                    text(s.vars[j]->name);
                }
                if (!s.rhs.empty()) {
                    text(" = ");
                    exprList(s.rhs);
                }
                endLine();
                break;
            }
            case SK::LocalFunction: {
                const std::string name = s.vars[0]->name;
                std::function<void()> pn = [&] { text(name); };
                function(*s.e->fn, "local function ", &pn);
                endLine();
                break;
            }
            case SK::Assign: {
                if (s.lhs.size() == 1 && s.rhs.size() == 1 && s.rhs[0]->k == EK::Function &&
                    (s.lhs[0]->k == EK::Global || (s.lhs[0]->k == EK::Index && FuncNamePath(s.lhs[0])))) {
                    const ExprP target = s.lhs[0];
                    std::function<void()> pn = [&] { expr(target); };
                    function(*s.rhs[0]->fn, "function ", &pn);
                    endLine();
                    break;
                }
                for (size_t j = 0; j < s.lhs.size(); ++j) {
                    if (j) text(", ");
                    expr(s.lhs[j]);
                }
                text(" = ");
                exprList(s.rhs);
                endLine();
                break;
            }
            case SK::Call:
                expr(s.e);
                endLine();
                break;
            case SK::Return:
                text("return");
                if (!s.rhs.empty()) {
                    text(" ");
                    args(s.rhs, true);
                }
                endLine();
                break;
            case SK::Break:
                text("break");
                endLine();
                break;
            case SK::Do:
                text("do");
                endLine(true);
                ++indent_;
                block(s.body);
                --indent_;
                text("end");
                endLine();
                break;
            case SK::While:
                text("while ");
                // `while "x" do`: the literal stays in the constant table.
                if (s.e->k == EK::True && nextIsUnref() &&
                    funcs_.back().f->proto->constants[size_t(nextNewK())].type == Constant::Type::String)
                    constantLiteral(nextNewK());
                else
                    expr(s.e);
                text(" do");
                endLine(true);
                ++indent_;
                block(s.body);
                --indent_;
                text("end");
                endLine();
                break;
            case SK::Repeat:
                text("repeat");
                endLine(true);
                ++indent_;
                block(s.body);
                --indent_;
                text("until ");
                expr(s.e);
                endLine();
                break;
            case SK::NumFor:
                text("for " + s.vars[0]->name + " = ");
                expr(s.rhs[0]);
                text(", ");
                expr(s.rhs[1]);
                if (s.rhs.size() > 2) {
                    text(", ");
                    expr(s.rhs[2]);
                } else {
                    markK(s.stepK);
                }
                text(" do");
                endLine(true);
                ++indent_;
                block(s.body);
                --indent_;
                text("end");
                endLine();
                break;
            case SK::GenFor:
                text("for ");
                for (size_t j = 0; j < s.vars.size(); ++j) {
                    if (j) text(", ");
                    text(s.vars[j]->name);
                }
                text(" in ");
                exprList(s.rhs);
                text(" do");
                endLine(true);
                ++indent_;
                block(s.body);
                --indent_;
                text("end");
                endLine();
                break;
            case SK::If: ifStmt(s, false); break;
            case SK::Comment:
                text("-- " + s.text);
                endLine();
                break;
        }
    }

    void ifStmt(const Stmt& s, bool asElseif) {
        for (size_t j = 0; j < s.conds.size(); ++j) {
            text(j == 0 && !asElseif ? "if " : "elseif ");
            expr(s.conds[j]);
            text(" then");
            endLine(true);
            ++indent_;
            block(s.blocks[j]);
            --indent_;
        }
        if (s.hasElse) {
            const Block& eb = s.blocks.back();
            if (eb.stmts.size() == 1 && eb.stmts[0]->k == SK::If) {
                ifStmt(*eb.stmts[0], true);
                return;
            }
            text("else");
            endLine(true);
            ++indent_;
            block(eb);
            --indent_;
        }
        text("end");
        endLine();
    }

    Decomp& d_;
    std::vector<Line> lines_;
    std::string cur_;
    int curAnchor_ = -1;
    int indent_ = 0;
    std::vector<FuncState> funcs_;
};

// ============================================================================
// Verification
// ============================================================================

// First difference between two functions' compiled form, ignoring debug info.
std::string CompareFunctions(const Function& a, const Function& b, const std::string& name) {
    if (a.params != b.params || a.vararg != b.vararg || a.upvalues != b.upvalues)
        return name + ": different parameters or upvalues";
    if (a.lineDefined != b.lineDefined) return name + ": defined on line " + std::to_string(b.lineDefined) +
                                                 " instead of " + std::to_string(a.lineDefined);
    if (a.lastLineDefined != b.lastLineDefined)
        return name + ": ends on line " + std::to_string(b.lastLineDefined) + " instead of " +
               std::to_string(a.lastLineDefined);
    const size_t n = std::min(a.code.size(), b.code.size());
    for (size_t i = 0; i < n; ++i)
        if (a.code[i] != b.code[i])
            return name + ": instruction " + std::to_string(i + 1) + " differs (" +
                   Name(GetOp(a.code[i])) + " vs " + Name(GetOp(b.code[i])) + ")";
    if (a.code.size() != b.code.size()) return name + ": " + std::to_string(b.code.size()) +
                                                " instructions instead of " + std::to_string(a.code.size());
    if (a.constants.size() != b.constants.size()) return name + ": different number of constants";
    for (size_t i = 0; i < a.constants.size(); ++i)
        if (!(a.constants[i] == b.constants[i])) return name + ": constant " + std::to_string(i) + " differs";
    if (a.maxStackSize != b.maxStackSize) return name + ": different register count";
    if (a.functions.size() != b.functions.size()) return name + ": different number of functions";
    for (size_t i = 0; i < a.functions.size(); ++i) {
        std::string d = CompareFunctions(a.functions[i], b.functions[i], name + "." + std::to_string(i));
        if (!d.empty()) return d;
    }
    return {};
}

} // namespace

DecompileResult Decompile51(const Chunk& chunk) {
    DecompileResult result;
    Decomp d;
    d.funcs.emplace_back();
    FuncAst& main = d.funcs.back();
    FuncDecoder dec(d, chunk.main, main);
    if (!dec.run({})) FuncDecoder::Unreadable(d, chunk.main, main, {});
    // Handlers are the functions the main chunk assigns to globals; their
    // parameters are (self, message, vars).
    for (const auto& s : main.body.stmts)
        if (s->k == SK::Assign && s->rhs.size() == 1 && s->rhs[0]->k == EK::Function && s->lhs[0]->k == EK::Global)
            s->rhs[0]->fn->handler = s->rhs[0]->fn->params.size() == 3;
    Namer(d).func(main, false);
    Printer printer(d);
    printer.chunk(main);
    result.source = printer.layout();
    result.warnings = d.warnings;

    // Check the source against the chunk it came from.
    CompileResult re = Compile51(result.source);
    if (!re.ok()) {
        result.warnings.push_back("the decompiled source does not compile: " + re.error);
    } else {
        try {
            const Chunk back = ReadChunk(re.chunk);
            const std::string diff = CompareFunctions(chunk.main, back.main, "main");
            if (!diff.empty()) result.warnings.push_back("recompiling does not reproduce the bytecode: " + diff);
            else result.exact = true;
        } catch (const std::exception& e) {
            result.warnings.push_back(std::string("recompiled chunk unreadable: ") + e.what());
        }
    }
    return result;
}

} // namespace attrib::lua
