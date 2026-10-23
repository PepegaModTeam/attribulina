#include "Chunk.h"

#include <cstring>

namespace attrib::lua {

namespace {

// 1B 'Lua', version 5.0, little-endian, int/size_t/Instruction 4 bytes,
// SIZE_OP/A/B/C 6/8/9/9, lua_Number 4 bytes.
constexpr uint8_t kHeader50[] = {0x1B, 'L', 'u', 'a', 0x50, 0x01, 0x04, 0x04, 0x04, 0x06, 0x08, 0x09, 0x09, 0x04};
// 1B 'Lua', version 5.1, official format, little-endian, int/size_t/Instruction/lua_Number 4, float.
constexpr uint8_t kHeader51[] = {0x1B, 'L', 'u', 'a', 0x51, 0x00, 0x01, 0x04, 0x04, 0x04, 0x04, 0x00};
constexpr float kTestNumber = float(3.14159265358979323846E7); // TEST_NUMBER (lundump.h)

class Reader50 {
public:
    explicit Reader50(ByteSpan data) : r_(data, Endian::Little) {}

    Chunk read() {
        if (r_.size() < sizeof(kHeader50) + 4 || std::memcmp(r_.data().data(), kHeader50, sizeof(kHeader50)) != 0)
            throw FormatError("not a Lua 5.0 chunk in the game format");
        pos_ = sizeof(kHeader50);
        const float test = r_.f32(pos_);
        if (std::memcmp(&test, &kTestNumber, 4) != 0)
            throw FormatError("Lua 5.0 chunk has an unexpected number format");
        pos_ += 4;
        Chunk c;
        c.version = Version::Lua50;
        c.main = function(0);
        if (pos_ != r_.size())
            throw FormatError("Lua chunk has " + std::to_string(r_.size() - pos_) + " trailing bytes");
        return c;
    }

private:
    uint8_t u8() { return r_.u8(pos_++); }
    int32_t i32() { int32_t v = int32_t(r_.u32(pos_)); pos_ += 4; return v; }
    uint32_t u32() { uint32_t v = r_.u32(pos_); pos_ += 4; return v; }
    float f32() { float v = r_.f32(pos_); pos_ += 4; return v; }
    int32_t count() {
        const int32_t n = i32();
        if (n < 0 || size_t(n) > r_.size() - pos_)
            throw FormatError("bad Lua chunk count " + std::to_string(n) + " at 0x" + ByteReader::Hex(pos_ - 4));
        return n;
    }
    std::optional<std::string> string() {
        const uint32_t n = u32();
        if (n == 0) return std::nullopt;
        ByteSpan s = r_.slice(pos_, n);
        if (s[n - 1] != 0)
            throw FormatError("unterminated Lua string at 0x" + ByteReader::Hex(pos_));
        pos_ += n;
        return std::string(reinterpret_cast<const char*>(s.data()), n - 1);
    }

    Function function(int depth) {
        if (depth > 200) throw FormatError("Lua functions nested too deeply");
        Function f;
        f.source = string();
        f.lineDefined = i32();
        f.upvalues = u8();
        f.params = u8();
        f.vararg = u8();
        f.maxStackSize = u8();
        f.lineInfo.resize(count());
        for (auto& l : f.lineInfo) l = i32();
        f.locals.resize(count());
        for (auto& l : f.locals) {
            l.name = string();
            l.startPc = i32();
            l.endPc = i32();
        }
        f.upvalueNames.resize(count());
        for (auto& u : f.upvalueNames) u = string();
        f.constants.resize(count());
        for (auto& k : f.constants) {
            const uint8_t t = u8();
            switch (t) {
                case 0: k.type = Constant::Type::Nil; break;
                case 3: k.type = Constant::Type::Number; k.number = f32(); break;
                case 4: {
                    k.type = Constant::Type::String;
                    auto s = string();
                    if (!s) throw FormatError("NULL string constant in Lua chunk");
                    k.string = std::move(*s);
                    break;
                }
                default: throw FormatError("bad Lua constant type " + std::to_string(t));
            }
        }
        const int32_t np = count();
        f.functions.reserve(np);
        for (int32_t i = 0; i < np; ++i) f.functions.push_back(function(depth + 1));
        f.code.resize(count());
        for (auto& i : f.code) i = u32();
        return f;
    }

    ByteReader r_;
    size_t pos_ = 0;
};

// Lua 5.1's ldump order: the header has no test number, the function
// header adds lastlinedefined, code comes first, constants (which may be
// booleans) are followed by the nested functions, and the debug information
// comes last.
class Reader51 {
public:
    explicit Reader51(ByteSpan data) : r_(data, Endian::Little) {}

    Chunk read() {
        if (r_.size() < sizeof(kHeader51) || std::memcmp(r_.data().data(), kHeader51, sizeof(kHeader51)) != 0)
            throw FormatError("not a Lua 5.1 chunk in the game format");
        pos_ = sizeof(kHeader51);
        Chunk c;
        c.version = Version::Lua51;
        c.main = function(0);
        if (pos_ != r_.size())
            throw FormatError("Lua chunk has " + std::to_string(r_.size() - pos_) + " trailing bytes");
        return c;
    }

private:
    uint8_t u8() { return r_.u8(pos_++); }
    int32_t i32() { int32_t v = int32_t(r_.u32(pos_)); pos_ += 4; return v; }
    uint32_t u32() { uint32_t v = r_.u32(pos_); pos_ += 4; return v; }
    float f32() { float v = r_.f32(pos_); pos_ += 4; return v; }
    int32_t count() {
        const int32_t n = i32();
        if (n < 0 || size_t(n) > r_.size() - pos_)
            throw FormatError("bad Lua chunk count " + std::to_string(n) + " at 0x" + ByteReader::Hex(pos_ - 4));
        return n;
    }
    std::optional<std::string> string() {
        const uint32_t n = u32();
        if (n == 0) return std::nullopt;
        ByteSpan s = r_.slice(pos_, n);
        if (s[n - 1] != 0)
            throw FormatError("unterminated Lua string at 0x" + ByteReader::Hex(pos_));
        pos_ += n;
        return std::string(reinterpret_cast<const char*>(s.data()), n - 1);
    }

    Function function(int depth) {
        if (depth > 200) throw FormatError("Lua functions nested too deeply");
        Function f;
        f.source = string();
        f.lineDefined = i32();
        f.lastLineDefined = i32();
        f.upvalues = u8();
        f.params = u8();
        f.vararg = u8();
        f.maxStackSize = u8();
        f.code.resize(count());
        for (auto& i : f.code) i = u32();
        f.constants.resize(count());
        for (auto& k : f.constants) {
            const uint8_t t = u8();
            switch (t) {
                case 0: k.type = Constant::Type::Nil; break;
                case 1:
                    k.type = Constant::Type::Boolean;
                    k.boolean = u8() != 0;
                    break;
                case 3: k.type = Constant::Type::Number; k.number = f32(); break;
                case 4: {
                    k.type = Constant::Type::String;
                    auto s = string();
                    if (!s) throw FormatError("NULL string constant in Lua chunk");
                    k.string = std::move(*s);
                    break;
                }
                default: throw FormatError("bad Lua constant type " + std::to_string(t));
            }
        }
        const int32_t np = count();
        f.functions.reserve(np);
        for (int32_t i = 0; i < np; ++i) f.functions.push_back(function(depth + 1));
        f.lineInfo.resize(count());
        for (auto& l : f.lineInfo) l = i32();
        f.locals.resize(count());
        for (auto& l : f.locals) {
            l.name = string();
            l.startPc = i32();
            l.endPc = i32();
        }
        f.upvalueNames.resize(count());
        for (auto& u : f.upvalueNames) u = string();
        return f;
    }

    ByteReader r_;
    size_t pos_ = 0;
};

class Writer50 {
public:
    Bytes write(const Chunk& c) {
        w_.raw(kHeader50, sizeof(kHeader50));
        float t = kTestNumber;
        uint32_t u;
        std::memcpy(&u, &t, 4);
        w_.u32(u);
        function(c.main);
        return std::move(w_.bytes());
    }

private:
    void i32(int32_t v) { w_.u32(uint32_t(v)); }
    void string(const std::optional<std::string>& s) {
        if (!s) {
            w_.u32(0);
            return;
        }
        w_.u32(uint32_t(s->size() + 1));
        w_.cstr(*s);
    }
    void function(const Function& f) {
        string(f.source);
        i32(f.lineDefined);
        w_.u8(f.upvalues);
        w_.u8(f.params);
        w_.u8(f.vararg);
        w_.u8(f.maxStackSize);
        i32(int32_t(f.lineInfo.size()));
        for (int32_t l : f.lineInfo) i32(l);
        i32(int32_t(f.locals.size()));
        for (const auto& l : f.locals) {
            string(l.name);
            i32(l.startPc);
            i32(l.endPc);
        }
        i32(int32_t(f.upvalueNames.size()));
        for (const auto& u : f.upvalueNames) string(u);
        i32(int32_t(f.constants.size()));
        for (const auto& k : f.constants) {
            switch (k.type) {
                case Constant::Type::Nil: w_.u8(0); break;
                case Constant::Type::Number: {
                    w_.u8(3);
                    uint32_t u;
                    std::memcpy(&u, &k.number, 4);
                    w_.u32(u);
                    break;
                }
                case Constant::Type::String:
                    w_.u8(4);
                    string(k.string);
                    break;
                case Constant::Type::Boolean: throw FormatError("Lua 5.0 chunks have no boolean constants");
            }
        }
        i32(int32_t(f.functions.size()));
        for (const auto& p : f.functions) function(p);
        i32(int32_t(f.code.size()));
        for (uint32_t i : f.code) w_.u32(i);
    }

    ByteWriter w_{Endian::Little};
};

class Writer51 {
public:
    Bytes write(const Chunk& c) {
        w_.raw(kHeader51, sizeof(kHeader51));
        function(c.main);
        return std::move(w_.bytes());
    }

private:
    void i32(int32_t v) { w_.u32(uint32_t(v)); }
    void string(const std::optional<std::string>& s) {
        if (!s) {
            w_.u32(0);
            return;
        }
        w_.u32(uint32_t(s->size() + 1));
        w_.cstr(*s);
    }
    void function(const Function& f) {
        string(f.source);
        i32(f.lineDefined);
        i32(f.lastLineDefined);
        w_.u8(f.upvalues);
        w_.u8(f.params);
        w_.u8(f.vararg);
        w_.u8(f.maxStackSize);
        i32(int32_t(f.code.size()));
        for (uint32_t i : f.code) w_.u32(i);
        i32(int32_t(f.constants.size()));
        for (const auto& k : f.constants) {
            switch (k.type) {
                case Constant::Type::Nil: w_.u8(0); break;
                case Constant::Type::Boolean:
                    w_.u8(1);
                    w_.u8(k.boolean ? 1 : 0);
                    break;
                case Constant::Type::Number: {
                    w_.u8(3);
                    uint32_t u;
                    std::memcpy(&u, &k.number, 4);
                    w_.u32(u);
                    break;
                }
                case Constant::Type::String:
                    w_.u8(4);
                    string(k.string);
                    break;
            }
        }
        i32(int32_t(f.functions.size()));
        for (const auto& p : f.functions) function(p);
        i32(int32_t(f.lineInfo.size()));
        for (int32_t l : f.lineInfo) i32(l);
        i32(int32_t(f.locals.size()));
        for (const auto& l : f.locals) {
            string(l.name);
            i32(l.startPc);
            i32(l.endPc);
        }
        i32(int32_t(f.upvalueNames.size()));
        for (const auto& u : f.upvalueNames) string(u);
    }

    ByteWriter w_{Endian::Little};
};

} // namespace

bool Constant::operator==(const Constant& o) const {
    if (type != o.type) return false;
    switch (type) {
        case Type::Nil: return true;
        case Type::Boolean: return boolean == o.boolean;
        case Type::Number: return std::memcmp(&number, &o.number, 4) == 0;
        case Type::String: return string == o.string;
    }
    return false;
}

std::optional<Version> DetectVersion(ByteSpan chunk) {
    if (chunk.size() >= sizeof(kHeader50) + 4 && std::memcmp(chunk.data(), kHeader50, sizeof(kHeader50)) == 0)
        return Version::Lua50;
    if (chunk.size() >= sizeof(kHeader51) && std::memcmp(chunk.data(), kHeader51, sizeof(kHeader51)) == 0)
        return Version::Lua51;
    return std::nullopt;
}

Chunk ReadChunk(ByteSpan data) {
    if (DetectVersion(data) == Version::Lua51) return Reader51(data).read();
    return Reader50(data).read();
}

Bytes WriteChunk(const Chunk& chunk) {
    if (chunk.version == Version::Lua51) return Writer51().write(chunk);
    return Writer50().write(chunk);
}

} // namespace attrib::lua
