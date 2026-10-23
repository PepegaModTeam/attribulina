// Runs the Lua 5.0.1 parser and code generator on source text and copies the
// resulting function prototypes out of the Lua state.
#include "lua50_config.h"

#include "lua50_compile.h"

namespace lua50 {
#include "src/lua.h"
#include "src/lobject.h"
#include "src/lstate.h"
#include "src/ldo.h"
} // namespace lua50

namespace lua50 {
namespace {

struct SourceReader {
    const char* data;
    size_t size;
    bool done;
};

const char* ReadSource(lua_State*, void* ud, size_t* size) {
    auto* r = static_cast<SourceReader*>(ud);
    if (r->done || r->size == 0) {
        *size = 0;
        return nullptr;
    }
    r->done = true;
    *size = r->size;
    return r->data;
}

void Copy(const Proto* p, CompiledFunction& out) {
    out.lineDefined = p->lineDefined;
    out.upvalues = p->nups;
    out.params = p->numparams;
    out.vararg = p->is_vararg != 0;
    out.maxStackSize = p->maxstacksize;
    out.code.assign(p->code, p->code + p->sizecode);
    out.constants.resize(p->sizek);
    for (int i = 0; i < p->sizek; ++i) {
        const TObject* o = &p->k[i];
        CompiledConstant& c = out.constants[i];
        if (ttisnumber(o)) {
            c.type = CompiledConstant::Number;
            c.number = nvalue(o);
        } else if (ttisstring(o)) {
            c.type = CompiledConstant::String;
            c.string.assign(getstr(tsvalue(o)), tsvalue(o)->tsv.len);
        } else {
            c.type = CompiledConstant::Nil;
        }
    }
    out.functions.resize(p->sizep);
    for (int i = 0; i < p->sizep; ++i)
        Copy(p->p[i], out.functions[i]);
}

} // namespace

bool CompileSource(const char* source, size_t size, const char* chunkName, CompiledFunction& out,
                   std::string& error) {
    out = CompiledFunction{};
    error.clear();
    // A leading ESC would make lua_load read the text as a binary chunk.
    if (size > 0 && source[0] == '\033') {
        error = std::string(chunkName) + ":1: source text expected, not a compiled chunk";
        return false;
    }
    lua_State* L = lua_open();
    if (!L) {
        error = std::string(chunkName) + ":0: not enough memory";
        return false;
    }
    SourceReader reader{source, size, false};
    const int status = lua_load(L, ReadSource, &reader, chunkName);
    bool ok = false;
    if (status == 0) {
        const TObject* top = L->top - 1;
        if (ttisfunction(top) && !clvalue(top)->c.isC) {
            Copy(clvalue(top)->l.p, out);
            ok = true;
        } else {
            error = std::string(chunkName) + ":0: compiler returned no function";
        }
    } else {
        const char* msg = lua_tostring(L, -1);
        error = msg ? msg : (std::string(chunkName) + ":0: compile error");
    }
    lua_close(L);
    return ok;
}

} // namespace lua50
