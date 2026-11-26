// SPDX-License-Identifier: GPL-2.0-or-later
#include "atlan/glsl.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace atlan::glsl {
namespace {
struct CompileError {
    std::string message;
    int line{};
};
[[noreturn]] void fail(const std::string& message, int line) { throw CompileError{message, line}; }

const char* base_prefix(Base b) {
    switch (b) {
        case Base::bool_: return "bool";
        case Base::int_: return "int";
        case Base::float_: return "float";
        default: return "void";
    }
}
const char* glsl_prefix(Base b) {
    switch (b) {
        case Base::bool_: return "b";
        case Base::int_: return "i";
        default: return "";
    }
}

// ---- lexer with a minimal preprocessor --------------------------------------------------------------------------
struct Tok {
    enum Kind : std::uint8_t { end, ident, integer, real, punct } kind{end};
    std::string text;
    int line{};
};

bool ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

class Lexer {
public:
    explicit Lexer(std::string_view source) : src_(source) { macros_["GL_ES"] = {}; }
    std::vector<Tok> run() {
        std::vector<Tok> out;
        lex(src_, out, true);
        if (!conditions_.empty()) fail("unterminated #if", line_);
        out.push_back({Tok::end, "", line_});
        return out;
    }
private:
    bool active() const { return std::all_of(conditions_.begin(), conditions_.end(), [](const Cond& c) { return c.on; }); }
    struct Cond { bool on; bool taken; };
    void lex(std::string_view s, std::vector<Tok>& out, bool directives) {
        std::size_t i = 0;
        bool line_start = true;
        while (i < s.size()) {
            const char c = s[i];
            if (c == '\n') { ++line_; ++i; line_start = true; continue; }
            if (c == ' ' || c == '\t' || c == '\r') { ++i; continue; }
            if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') { while (i < s.size() && s[i] != '\n') ++i; continue; }
            if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
                i += 2;
                while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) { if (s[i] == '\n') ++line_; ++i; }
                if (i + 1 >= s.size()) fail("unterminated comment", line_);
                i += 2;
                continue;
            }
            if (c == '#' && directives && line_start) {
                std::size_t end = i;
                std::string text;
                while (end < s.size() && s[end] != '\n') {
                    if (s[end] == '\\' && end + 1 < s.size() && s[end + 1] == '\n') { end += 2; ++line_; continue; }
                    // strip trailing line comments from the directive
                    if (s[end] == '/' && end + 1 < s.size() && s[end + 1] == '/') { while (end < s.size() && s[end] != '\n') ++end; break; }
                    text += s[end++];
                }
                directive(text);
                i = end;
                continue;
            }
            line_start = false;
            if (!active()) { ++i; continue; }
            if (ident_start(c)) {
                std::size_t e = i;
                while (e < s.size() && ident_char(s[e])) ++e;
                std::string word(s.substr(i, e - i));
                i = e;
                if (const auto m = macros_.find(word); m != macros_.end() && !expanding_) {
                    for (auto t : m->second) { t.line = line_; out.push_back(std::move(t)); }
                } else {
                    out.push_back({Tok::ident, std::move(word), line_});
                }
                continue;
            }
            const bool digit = std::isdigit(static_cast<unsigned char>(c));
            if (digit || (c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
                std::size_t e = i;
                bool real = false;
                if (c == '0' && e + 1 < s.size() && (s[e + 1] == 'x' || s[e + 1] == 'X')) {
                    e += 2;
                    while (e < s.size() && std::isxdigit(static_cast<unsigned char>(s[e]))) ++e;
                } else {
                    while (e < s.size() && std::isdigit(static_cast<unsigned char>(s[e]))) ++e;
                    if (e < s.size() && s[e] == '.') { real = true; ++e; while (e < s.size() && std::isdigit(static_cast<unsigned char>(s[e]))) ++e; }
                    if (e < s.size() && (s[e] == 'e' || s[e] == 'E')) {
                        std::size_t k = e + 1;
                        if (k < s.size() && (s[k] == '+' || s[k] == '-')) ++k;
                        if (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) {
                            real = true;
                            e = k;
                            while (e < s.size() && std::isdigit(static_cast<unsigned char>(s[e]))) ++e;
                        }
                    }
                }
                out.push_back({real ? Tok::real : Tok::integer, std::string(s.substr(i, e - i)), line_});
                i = e;
                continue;
            }
            static constexpr const char* two[] = {"++", "--", "+=", "-=", "*=", "/=", "==", "!=", "<=", ">=", "&&", "||", "^^", "<<", ">>"};
            bool matched = false;
            if (i + 1 < s.size()) {
                for (const char* t : two) {
                    if (s[i] == t[0] && s[i + 1] == t[1]) { out.push_back({Tok::punct, t, line_}); i += 2; matched = true; break; }
                }
            }
            if (matched) continue;
            if (std::string_view("+-*/%<>=!&|^~?:;,.()[]{}").find(c) == std::string_view::npos)
                fail(std::string("unexpected character '") + c + "'", line_);
            out.push_back({Tok::punct, std::string(1, c), line_});
            ++i;
        }
    }
    void directive(const std::string& text) {
        std::size_t p = 1;
        while (p < text.size() && std::isspace(static_cast<unsigned char>(text[p]))) ++p;
        std::size_t e = p;
        while (e < text.size() && ident_char(text[e])) ++e;
        const std::string name = text.substr(p, e - p);
        std::string rest = text.substr(e);
        while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) rest.erase(rest.begin());
        while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.back()))) rest.pop_back();
        if (name == "ifdef" || name == "ifndef") {
            const bool defined = macros_.contains(rest);
            const bool on = name == "ifdef" ? defined : !defined;
            conditions_.push_back({on, on});
            return;
        }
        if (name == "else") {
            if (conditions_.empty()) fail("#else without #if", line_);
            auto& c = conditions_.back();
            c.on = !c.taken;
            c.taken = true;
            return;
        }
        if (name == "endif") {
            if (conditions_.empty()) fail("#endif without #if", line_);
            conditions_.pop_back();
            return;
        }
        if (!active()) return;
        if (name == "version" || name == "extension" || name == "pragma" || name == "line" || name.empty()) return;
        if (name == "define") {
            std::size_t k = 0;
            while (k < rest.size() && ident_char(rest[k])) ++k;
            const std::string macro = rest.substr(0, k);
            if (macro.empty()) fail("#define needs a name", line_);
            if (k < rest.size() && rest[k] == '(') fail("function-like macros are not supported", line_);
            std::vector<Tok> body;
            expanding_ = true;
            lex(std::string_view(rest).substr(k), body, false);
            expanding_ = false;
            macros_[macro] = std::move(body);
            return;
        }
        if (name == "undef") { macros_.erase(rest); return; }
        fail("unsupported preprocessor directive #" + name, line_);
    }
    std::string_view src_;
    int line_{1};
    bool expanding_{};
    std::map<std::string, std::vector<Tok>> macros_;
    std::vector<Cond> conditions_;
};

// ---- identifiers -----------------------------------------------------------------------------------------------
std::string safe(const std::string& name) {
    static const std::unordered_set<std::string> reserved = {
        "alignas", "alignof", "and", "asm", "auto", "bitand", "bitor", "catch", "char", "class", "compl", "constexpr", "const_cast",
        "constant", "decltype", "default", "delete", "device", "dynamic_cast", "explicit", "export", "extern", "final", "friend",
        "fragment", "goto", "half", "inline", "kernel", "long", "mutable", "namespace", "new", "noexcept", "not", "nullptr",
        "operator", "or", "override", "private", "protected", "public", "register", "reinterpret_cast", "sample", "short",
        "signed", "sizeof", "static", "static_cast", "struct", "template", "this", "thread", "threadgroup", "throw", "try",
        "typedef", "typeid", "typename", "union", "unsigned", "using", "vertex", "virtual", "volatile", "wchar_t", "xor",
        "metal", "sampler", "texture2d", "uint", "uchar", "ushort", "ulong", "main", "switch", "case", "float2", "float3",
        "float4", "int2", "int3", "int4", "bool2", "bool3", "bool4", "float2x2", "float3x3", "float4x4", "tex", "u", "a", "o", "v", "f"};
    if (reserved.contains(name) || name.starts_with("_") || name.find("__") != std::string::npos) return name + "_g";
    return name;
}

// ---- types -----------------------------------------------------------------------------------------------------
std::optional<Type> type_from_name(const std::string& n) {
    if (n == "void") return Type{Base::void_, 1, 1, 0};
    if (n == "float") return Type{Base::float_, 1, 1, 0};
    if (n == "int") return Type{Base::int_, 1, 1, 0};
    if (n == "bool") return Type{Base::bool_, 1, 1, 0};
    if (n == "sampler2D") return Type{Base::sampler2d, 1, 1, 0};
    if (n.size() == 4 && n.starts_with("vec") && n[3] >= '2' && n[3] <= '4') return Type{Base::float_, std::uint8_t(n[3] - '0'), 1, 0};
    if (n.size() == 5 && n.starts_with("ivec") && n[4] >= '2' && n[4] <= '4') return Type{Base::int_, std::uint8_t(n[4] - '0'), 1, 0};
    if (n.size() == 5 && n.starts_with("bvec") && n[4] >= '2' && n[4] <= '4') return Type{Base::bool_, std::uint8_t(n[4] - '0'), 1, 0};
    if (n.size() == 4 && n.starts_with("mat") && n[3] >= '2' && n[3] <= '4') return Type{Base::float_, std::uint8_t(n[3] - '0'), std::uint8_t(n[3] - '0'), 0};
    return std::nullopt;
}

std::uint32_t msl_size(const Type& t) {  // sizeof the element type as the uniform block stores it
    const Type e = t.element();
    if (e.is_matrix()) return e.cols * (e.rows == 2 ? 8u : 16u);
    if (e.is_vector()) return e.rows == 2 ? 8u : 16u;
    return 4;
}
std::string storage_msl(const Type& t) {  // bool uniforms are stored as int (glUniform1i writes them)
    Type e = t.element();
    if (e.base == Base::bool_) e.base = Base::int_;
    return e.msl();
}
}

std::string Type::msl() const {
    if (base == Base::sampler2d) return "texture2d<float>";
    if (base == Base::void_) return "void";
    std::string s = base_prefix(base);
    if (cols > 1) return s + std::to_string(cols) + "x" + std::to_string(rows);
    if (rows > 1) s += std::to_string(rows);
    return s;
}
std::string Type::glsl() const {
    if (base == Base::sampler2d) return "sampler2D";
    if (base == Base::void_) return "void";
    if (cols > 1) return "mat" + std::to_string(cols);
    if (rows > 1) return std::string(glsl_prefix(base)) + "vec" + std::to_string(rows);
    return base_prefix(base);
}

namespace {
// ---- parser / emitter ------------------------------------------------------------------------------------------
struct Expr {
    std::string text;
    Type type;
    bool sampler{};
    std::string sampler_name;  // GLSL name when `sampler`
};

enum class SymKind : std::uint8_t { local, uniform, attribute, varying, sampler, builtin, constant };
struct Sym {
    SymKind kind{};
    Type type;
    std::string text;   // MSL expression that names it
    std::string glsl;   // original name
};

struct FnSig {
    std::string name;       // MSL name
    Type ret;
    std::vector<Type> params;
    std::size_t hidden_samplers{};
};

class Parser {
public:
    Parser(Stage stage, std::vector<Tok> tokens) : stage_(stage), toks_(std::move(tokens)) { scopes_.emplace_back(); builtins(); }

    void run(ShaderUnit& unit) {
        unit_ = &unit;
        while (peek().kind != Tok::end) global_declaration();
        if (!has_main_) fail("missing main()", peek().line);
    }

private:
    // token helpers
    [[nodiscard]] const Tok& peek(std::size_t ahead = 0) const { return toks_[std::min(pos_ + ahead, toks_.size() - 1)]; }
    const Tok& next() { const Tok& t = toks_[pos_]; if (pos_ + 1 < toks_.size()) ++pos_; return t; }
    [[nodiscard]] bool is(const char* p, std::size_t ahead = 0) const { const auto& t = peek(ahead); return (t.kind == Tok::punct || t.kind == Tok::ident) && t.text == p; }
    bool accept(const char* p) { if (is(p)) { next(); return true; } return false; }
    void expect(const char* p) { if (!accept(p)) fail(std::string("expected '") + p + "' but found '" + peek().text + "'", peek().line); }
    std::string ident() {
        if (peek().kind != Tok::ident) fail("expected an identifier but found '" + peek().text + "'", peek().line);
        return next().text;
    }

    void builtins() {
        if (stage_ == Stage::vertex) scopes_[0]["gl_Position"] = {SymKind::builtin, {Base::float_, 4, 1, 0}, "o.gl_Position", "gl_Position"};
        else scopes_[0]["gl_FragColor"] = {SymKind::builtin, {Base::float_, 4, 1, 0}, "f.gl_FragColor", "gl_FragColor"};
    }

    // scopes
    void push() { scopes_.emplace_back(); }
    void pop() { scopes_.pop_back(); }
    const Sym* lookup(const std::string& n) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
            if (const auto f = it->find(n); f != it->end()) return &f->second;
        return nullptr;
    }
    void declare(const std::string& n, Sym s, int line) {
        auto& scope = scopes_.back();
        if (scope.contains(n)) fail("'" + n + "' is already declared", line);
        scope.emplace(n, std::move(s));
    }

    // ---- type parsing
    struct Specifier {
        Type type;
        bool is_const{}, attribute{}, varying{}, uniform{};
        int param_qualifier{};  // 0 in, 1 out, 2 inout
    };
    [[nodiscard]] bool at_type_start() const {
        const auto& t = peek();
        if (t.kind != Tok::ident) return false;
        static const std::unordered_set<std::string> q = {"const", "attribute", "varying", "uniform", "lowp", "mediump", "highp", "invariant", "in", "out", "inout"};
        return q.contains(t.text) || type_from_name(t.text).has_value();
    }
    Specifier specifier() {
        Specifier s;
        for (;;) {
            const auto& t = peek();
            if (t.kind != Tok::ident) break;
            if (t.text == "const") s.is_const = true;
            else if (t.text == "attribute") s.attribute = true;
            else if (t.text == "varying") s.varying = true;
            else if (t.text == "uniform") s.uniform = true;
            else if (t.text == "in") s.param_qualifier = 0;
            else if (t.text == "out") s.param_qualifier = 1;
            else if (t.text == "inout") s.param_qualifier = 2;
            else if (t.text == "lowp" || t.text == "mediump" || t.text == "highp" || t.text == "invariant") {}
            else break;
            next();
        }
        const auto line = peek().line;
        const auto name = ident();
        const auto type = type_from_name(name);
        if (!type) {
            if (name == "struct") fail("struct types are not supported", line);
            fail("unknown type '" + name + "'", line);
        }
        s.type = *type;
        return s;
    }
    std::uint16_t array_suffix() {
        if (!accept("[")) return 0;
        const auto& t = next();
        if (t.kind != Tok::integer) fail("array size must be an integer literal", t.line);
        const auto n = std::strtoul(t.text.c_str(), nullptr, 0);
        if (n == 0 || n > 1024) fail("array size out of range", t.line);
        expect("]");
        return static_cast<std::uint16_t>(n);
    }

    // ---- global scope
    void global_declaration() {
        if (is("precision")) {
            while (!is(";") && peek().kind != Tok::end) next();
            expect(";");
            return;
        }
        if (is("invariant") && peek(1).kind == Tok::ident && !type_from_name(peek(1).text)) {  // invariant gl_Position;
            while (!is(";") && peek().kind != Tok::end) next();
            expect(";");
            return;
        }
        const auto line = peek().line;
        Specifier spec = specifier();
        const std::string name = ident();
        if (is("(")) {
            if (spec.attribute || spec.varying || spec.uniform) fail("a function cannot have a storage qualifier", line);
            function(spec.type, name, line);
            return;
        }
        for (bool first = true;; first = false) {
            const std::string var = first ? name : ident();
            Type type = spec.type;
            type.array = array_suffix();
            variable(spec, var, type, line);
            if (accept(",")) continue;
            expect(";");
            break;
        }
    }
    void variable(const Specifier& spec, const std::string& name, const Type& type, int line) {
        if (type.base == Base::void_) fail("variable cannot be void", line);
        if (spec.uniform) {
            if (type.base == Base::sampler2d) {
                if (stage_ == Stage::vertex) fail("sampler uniforms are not supported in vertex shaders", line);
                if (type.array) fail("sampler arrays are not supported", line);
                unit_->samplers.push_back({name, type});
                declare(name, {SymKind::sampler, type, safe(name), name}, line);
                return;
            }
            unit_->uniforms.push_back({name, type});
            declare(name, {SymKind::uniform, type, "u." + safe(name), name}, line);
            return;
        }
        if (spec.attribute) {
            if (stage_ != Stage::vertex) fail("attributes are only allowed in vertex shaders", line);
            if (type.base != Base::float_ || type.is_matrix() || type.array) fail("unsupported attribute type", line);
            unit_->attributes.push_back({name, type});
            declare(name, {SymKind::attribute, type, "a." + safe(name), name}, line);
            return;
        }
        if (spec.varying) {
            if (type.base != Base::float_) fail("varyings must be float based", line);
            if (type.array || type.is_matrix()) fail("array and matrix varyings are not supported", line);
            unit_->varyings.push_back({name, type});
            declare(name, {SymKind::varying, type, (stage_ == Stage::vertex ? "o." : "v.") + safe(name), name}, line);
            return;
        }
        if (spec.is_const) {
            std::string init;
            if (accept("=")) init = " = " + convert(assignment(), type).text;
            else fail("a const needs an initializer", line);
            unit_->functions += "constant " + type.msl() + " " + safe(name) + (type.array ? "[" + std::to_string(type.array) + "]" : "") + init + ";\n";
            declare(name, {SymKind::constant, type, safe(name), name}, line);
            return;
        }
        fail("global variables must be uniform, attribute, varying or const", line);
    }

    // ---- functions
    std::string hidden_params(std::size_t samplers) const {
        std::string s = "constant Uniforms& u, ";
        if (stage_ == Stage::vertex) s += "thread VIn& a, thread VOut& o";
        else {
            s += "thread VOut& v, thread FOut& f";
            for (std::size_t i = 0; i < samplers; ++i) {
                const auto n = safe(unit_->samplers[i].name);
                s += ", texture2d<float> t_" + n + ", sampler m_" + n;
            }
        }
        return s;
    }
    std::string hidden_args(std::size_t samplers) const {
        std::string s = "u, ";
        if (stage_ == Stage::vertex) s += "a, o";
        else {
            s += "v, f";
            for (std::size_t i = 0; i < samplers; ++i) {
                const auto n = safe(unit_->samplers[i].name);
                s += ", t_" + n + ", m_" + n;
            }
        }
        return s;
    }
    void function(const Type& ret, const std::string& name, int line) {
        expect("(");
        struct Param { Type type; std::string name; int qualifier{}; };
        std::vector<Param> params;
        if (is("void") && is(")", 1)) next();
        else if (!is(")")) {
            for (;;) {
                Specifier s = specifier();
                Param p{s.type, {}, s.param_qualifier};
                if (peek().kind == Tok::ident) p.name = ident();
                p.type.array = array_suffix();
                if (p.type.base == Base::sampler2d) fail("sampler parameters are not supported", line);
                params.push_back(std::move(p));
                if (!accept(",")) break;
            }
        }
        expect(")");
        const bool is_main = name == "main";
        if (is_main && (!params.empty() || ret.base != Base::void_)) fail("main must be 'void main()'", line);
        FnSig sig;
        sig.name = (stage_ == Stage::vertex ? "vs_" : "fs_") + (is_main ? std::string("user_main") : safe(name));
        sig.ret = ret;
        for (const auto& p : params) sig.params.push_back(p.type);
        sig.hidden_samplers = stage_ == Stage::fragment ? unit_->samplers.size() : 0;
        FnSig* existing = nullptr;
        for (auto& f : fns_[name]) if (f.params == sig.params) existing = &f;
        if (existing) sig.hidden_samplers = existing->hidden_samplers;
        else fns_[name].push_back(sig);
        std::string head = ret.msl() + " " + sig.name + "(";
        for (std::size_t i = 0; i < params.size(); ++i) {
            const auto& p = params[i];
            head += p.qualifier == 0 ? p.type.msl() + " " : "thread " + p.type.msl() + "& ";
            head += (p.name.empty() ? "_p" + std::to_string(i) : safe(p.name));
            if (p.type.array) head += "[" + std::to_string(p.type.array) + "]";
            head += ", ";
        }
        head += hidden_params(sig.hidden_samplers) + ")";
        if (accept(";")) { unit_->functions += head + ";\n"; return; }
        if (is_main) has_main_ = true;
        expect("{");
        push();
        for (const auto& p : params) if (!p.name.empty()) declare(p.name, {SymKind::local, p.type, safe(p.name), p.name}, line);
        std::string body;
        in_main_ = is_main;
        while (!is("}")) {
            if (peek().kind == Tok::end) fail("unexpected end of shader inside a function", peek().line);
            statement(body);
        }
        in_main_ = false;
        expect("}");
        pop();
        unit_->functions += head + " {\n" + body + "}\n";
    }

    // ---- statements
    void statement(std::string& out) {
        const auto line = peek().line;
        if (accept("{")) {
            push();
            out += "{\n";
            while (!is("}")) {
                if (peek().kind == Tok::end) fail("missing '}'", line);
                statement(out);
            }
            expect("}");
            pop();
            out += "}\n";
            return;
        }
        if (accept(";")) { out += ";\n"; return; }
        if (accept("if")) {
            expect("(");
            Expr c = expression();
            expect(")");
            require_bool(c, line);
            out += "if " + condition(c.text) + " ";
            scoped_statement(out);
            if (accept("else")) { out += "else "; scoped_statement(out); }
            return;
        }
        if (accept("for")) {
            push();
            expect("(");
            std::string init, cond, inc;
            if (!is(";")) init = at_type_start() ? local_declaration() : expression().text;
            expect(";");
            if (!is(";")) { Expr c = expression(); require_bool(c, line); cond = c.text; }
            expect(";");
            if (!is(")")) inc = expression().text;
            expect(")");
            out += "for (" + init + "; " + cond + "; " + inc + ") ";
            scoped_statement(out);
            pop();
            return;
        }
        if (accept("while")) {
            push();
            expect("(");
            Expr c = expression();
            expect(")");
            require_bool(c, line);
            out += "while " + condition(c.text) + " ";
            scoped_statement(out);
            pop();
            return;
        }
        if (accept("do")) {
            out += "do ";
            scoped_statement(out);
            expect("while");
            expect("(");
            Expr c = expression();
            expect(")");
            expect(";");
            require_bool(c, line);
            out += "while " + condition(c.text) + ";\n";
            return;
        }
        if (accept("return")) {
            if (accept(";")) { out += "return;\n"; return; }
            out += "return " + expression().text + ";\n";
            expect(";");
            return;
        }
        if (accept("break")) { expect(";"); out += "break;\n"; return; }
        if (accept("continue")) { expect(";"); out += "continue;\n"; return; }
        if (accept("discard")) {
            if (stage_ != Stage::fragment || !in_main_) fail("discard is only supported directly in a fragment main()", line);
            expect(";");
            out += "{ discard_fragment(); return; }\n";
            return;
        }
        if (is("precision")) {
            while (!is(";") && peek().kind != Tok::end) next();
            expect(";");
            return;
        }
        if (at_type_start() && !(peek().kind == Tok::ident && type_from_name(peek().text) && is("(", 1))) {
            out += local_declaration() + ";\n";
            expect(";");
            return;
        }
        out += expression().text + ";\n";
        expect(";");
    }
    void scoped_statement(std::string& out) {
        push();
        if (is("{")) statement(out);
        else { out += "{\n"; statement(out); out += "}\n"; }
        pop();
    }
    std::string local_declaration() {  // consumes through the last declarator, not the ';'
        const auto line = peek().line;
        Specifier spec = specifier();
        if (spec.type.base == Base::void_ || spec.type.base == Base::sampler2d) fail("invalid local variable type", line);
        std::string out;
        for (bool first = true;; first = false) {
            const std::string name = ident();
            Type type = spec.type;
            type.array = array_suffix();
            std::string init;
            if (accept("=")) init = " = " + convert(assignment(), type).text;
            else if (spec.is_const) fail("a const needs an initializer", line);
            if (first) out += (spec.is_const ? "const " : "") + type.msl() + " ";
            else out += ", ";
            out += safe(name) + (type.array ? "[" + std::to_string(type.array) + "]" : "") + init;
            declare(name, {SymKind::local, type, safe(name), name}, line);
            if (!accept(",")) break;
        }
        // MSL declares one type per statement; "T a = x, b = y" is valid C++ for the same type.
        return out;
    }
    static std::string condition(const std::string& text) {
        if (text.size() >= 2 && text.front() == '(' && text.back() == ')') {
            int depth = 0;
            for (std::size_t i = 0; i < text.size(); ++i) {
                depth += text[i] == '(' ? 1 : text[i] == ')' ? -1 : 0;
                if (depth == 0 && i + 1 < text.size()) return "(" + text + ")";  // the first '(' closes early
            }
            return text;
        }
        return "(" + text + ")";
    }
    static void require_bool(const Expr& e, int line) {
        if (e.type.base != Base::bool_ || !e.type.is_scalar()) fail("condition must be a bool", line);
    }

    // ---- expression helpers
    static Expr make(std::string text, Type type) { return Expr{std::move(text), type, false, {}}; }
    static Type vec_of(Base b, unsigned n) { return Type{b, std::uint8_t(n), 1, 0}; }
    static std::string swizzle(const std::string& text, unsigned n) { return "(" + text + ")." + std::string("xyzw").substr(0, n); }
    // Lenient int -> float conversion where a float operand is required (some GLSL ES drivers accept it).
    static Expr to_float(Expr e) {
        if (e.type.base == Base::int_ || e.type.base == Base::bool_) {
            e.text = Type{Base::float_, e.type.rows, e.type.cols, 0}.msl() + "(" + e.text + ")";
            e.type.base = Base::float_;
        }
        return e;
    }
    static Expr convert(Expr e, const Type& target) {
        if (e.sampler) return e;
        if (target.base == Base::float_ && e.type.base != Base::float_ && e.type.components() == target.components() && !target.array) return to_float(std::move(e));
        return e;
    }

    // ---- expressions
    Expr expression() {
        Expr e = assignment();
        while (accept(",")) {
            Expr r = assignment();
            e = make("(" + e.text + ", " + r.text + ")", r.type);
        }
        return e;
    }
    Expr assignment() {
        const auto line = peek().line;
        Expr lhs = conditional();
        for (const char* op : {"=", "+=", "-=", "*=", "/="}) {
            if (is(op)) {
                next();
                Expr rhs = assignment();
                if (lhs.sampler) fail("cannot assign to a sampler", line);
                if (std::string(op) != "=") rhs = (lhs.type.base == Base::float_) ? to_float(std::move(rhs)) : std::move(rhs);
                else rhs = convert(std::move(rhs), lhs.type);
                if (std::string(op) == "=" && lhs.type.components() != rhs.type.components() && !(lhs.type.array || rhs.type.array))
                    fail("cannot assign a " + rhs.type.glsl() + " to a " + lhs.type.glsl(), line);
                return make("(" + lhs.text + " " + op + " " + rhs.text + ")", lhs.type);
            }
        }
        return lhs;
    }
    Expr conditional() {
        Expr c = binary(0);
        if (!accept("?")) return c;
        const auto line = peek().line;
        require_bool(c, line);
        Expr a = expression();
        expect(":");
        Expr b = assignment();
        if (a.type.base != b.type.base) { a = to_float(std::move(a)); b = to_float(std::move(b)); }
        return make("(" + c.text + " ? " + a.text + " : " + b.text + ")", a.type);
    }

    static int precedence(const std::string& op) {
        if (op == "||") return 1;
        if (op == "^^") return 2;
        if (op == "&&") return 3;
        if (op == "==" || op == "!=") return 4;
        if (op == "<" || op == ">" || op == "<=" || op == ">=") return 5;
        if (op == "+" || op == "-") return 6;
        if (op == "*" || op == "/") return 7;
        return -1;
    }
    Expr binary(int min_precedence) {
        Expr lhs = unary();
        for (;;) {
            const auto& t = peek();
            if (t.kind != Tok::punct) break;
            const int p = precedence(t.text);
            if (p < 0 || p < min_precedence) break;
            const std::string op = next().text;
            const auto line = t.line;
            Expr rhs = binary(p + 1);
            lhs = combine(op, std::move(lhs), std::move(rhs), line);
        }
        return lhs;
    }
    Expr combine(const std::string& op, Expr a, Expr b, int line) {
        if (a.sampler || b.sampler) fail("a sampler can only be used as a texture2D argument", line);
        if (op == "&&" || op == "||" || op == "^^") {
            require_bool(a, line);
            require_bool(b, line);
            return make("(" + a.text + (op == "^^" ? " != " : " " + op + " ") + b.text + ")", Type{Base::bool_, 1, 1, 0});
        }
        if (op == "==" || op == "!=") {
            if (a.type.base != b.type.base) { a = to_float(std::move(a)); b = to_float(std::move(b)); }
            if (a.type.components() != b.type.components() || a.type.array || b.type.array) fail("cannot compare " + a.type.glsl() + " with " + b.type.glsl(), line);
            const Type boolean{Base::bool_, 1, 1, 0};
            if (a.type.is_matrix()) return make(std::string("(") + (op == "!=" ? "!" : "") + "_glsl_meq(" + a.text + ", " + b.text + "))", boolean);
            if (a.type.is_vector()) return make(std::string("(") + (op == "==" ? "all" : "any") + "(" + a.text + " " + op + " " + b.text + "))", boolean);
            return make("(" + a.text + " " + op + " " + b.text + ")", boolean);
        }
        if (op == "<" || op == ">" || op == "<=" || op == ">=") {
            if (!a.type.is_scalar() || !b.type.is_scalar()) fail("relational operators need scalars", line);
            if (a.type.base != b.type.base) { a = to_float(std::move(a)); b = to_float(std::move(b)); }
            return make("(" + a.text + " " + op + " " + b.text + ")", Type{Base::bool_, 1, 1, 0});
        }
        // arithmetic
        if (a.type.base != b.type.base) {
            if (a.type.base == Base::float_ || b.type.base == Base::float_) { a = to_float(std::move(a)); b = to_float(std::move(b)); }
        }
        if (a.type.array || b.type.array) fail("arithmetic on arrays", line);
        const Base base = a.type.base;
        Type result;
        const Type &x = a.type, &y = b.type;
        if (x.is_scalar() && y.is_scalar()) result = x;
        else if (x.is_scalar()) result = y;
        else if (y.is_scalar()) result = x;
        else if (op == "*" && x.is_matrix() && y.is_vector()) {
            if (x.cols != y.rows) fail("matrix and vector sizes do not match", line);
            result = vec_of(base, x.rows);
        } else if (op == "*" && x.is_vector() && y.is_matrix()) {
            if (y.rows != x.rows) fail("vector and matrix sizes do not match", line);
            result = vec_of(base, y.cols);
        } else if (op == "*" && x.is_matrix() && y.is_matrix()) {
            if (x.cols != y.rows) fail("matrix sizes do not match", line);
            result = x;
        } else if (x.rows == y.rows && x.cols == y.cols) result = x;
        else fail("operands of '" + op + "' have different sizes (" + x.glsl() + ", " + y.glsl() + ")", line);
        return make("(" + a.text + " " + op + " " + b.text + ")", result);
    }
    Expr unary() {
        const auto line = peek().line;
        if (is("-") || is("+") || is("!") || is("++") || is("--")) {
            const std::string op = next().text;
            Expr e = unary();
            if (e.sampler) fail("invalid operand", line);
            if (op == "!") { require_bool(e, line); return make("(!" + e.text + ")", e.type); }
            return make("(" + op + e.text + ")", e.type);
        }
        return postfix();
    }
    Expr postfix() {
        Expr e = primary();
        for (;;) {
            const auto line = peek().line;
            if (accept("[")) {
                Expr i = expression();
                expect("]");
                if (e.sampler) fail("cannot index a sampler", line);
                Type t;
                if (e.type.array) t = e.type.element();
                else if (e.type.is_matrix()) t = vec_of(e.type.base, e.type.rows);
                else if (e.type.is_vector()) t = Type{e.type.base, 1, 1, 0};
                else fail("cannot index a " + e.type.glsl(), line);
                e = make(e.text + "[" + i.text + "]", t);
            } else if (accept(".")) {
                const std::string field = ident();
                if (e.sampler || !(e.type.is_vector() || e.type.is_scalar())) fail("'." + field + "' needs a vector", line);
                if (field.size() > 4) fail("invalid swizzle '" + field + "'", line);
                const std::string sets[3] = {"xyzw", "rgba", "stpq"};
                std::string mapped;
                int which = -1;
                for (char c : field) {
                    int found = -1;
                    for (int s = 0; s < 3; ++s) if (sets[s].find(c) != std::string::npos) found = s;
                    if (found < 0 || (which >= 0 && found != which)) fail("invalid swizzle '" + field + "'", line);
                    which = found;
                    const auto idx = sets[found].find(c);
                    if (idx >= e.type.rows) fail("swizzle '" + field + "' exceeds the vector size", line);
                    mapped += "xyzw"[idx];
                }
                e = make(e.text + "." + mapped, field.size() == 1 ? Type{e.type.base, 1, 1, 0} : vec_of(e.type.base, unsigned(field.size())));
            } else if (is("++") || is("--")) {
                e = make("(" + e.text + next().text + ")", e.type);
            } else {
                break;
            }
        }
        return e;
    }
    std::vector<Expr> arguments() {
        std::vector<Expr> args;
        expect("(");
        if (!is(")")) {
            for (;;) {
                args.push_back(assignment());
                if (!accept(",")) break;
            }
        }
        expect(")");
        return args;
    }
    Expr primary() {
        const auto& t = peek();
        const auto line = t.line;
        if (t.kind == Tok::integer) { next(); return make(t.text, Type{Base::int_, 1, 1, 0}); }
        if (t.kind == Tok::real) {
            next();
            return make(t.text + "f", Type{Base::float_, 1, 1, 0});
        }
        if (accept("(")) {
            Expr e = expression();
            expect(")");
            if (e.sampler) return e;
            return make("(" + e.text + ")", e.type);
        }
        if (t.kind != Tok::ident) fail("unexpected '" + t.text + "'", line);
        if (t.text == "true" || t.text == "false") { next(); return make(t.text, Type{Base::bool_, 1, 1, 0}); }
        const std::string name = next().text;
        if (const auto type = type_from_name(name)) {
            if (!is("(")) fail("a type name must be followed by '('", line);
            return construct(*type, arguments(), line);
        }
        if (is("(")) return call(name, arguments(), line);
        const Sym* sym = lookup(name);
        if (!sym) fail("undeclared identifier '" + name + "'", line);
        if (sym->kind == SymKind::sampler) { Expr e = make(sym->text, sym->type); e.sampler = true; e.sampler_name = sym->glsl; return e; }
        return make(sym->text, sym->type);
    }

    // ---- constructors
    Expr construct(const Type& target, const std::vector<Expr>& args, int line) {
        if (target.base == Base::void_ || target.base == Base::sampler2d) fail("cannot construct this type", line);
        if (args.empty()) fail("constructor needs arguments", line);
        for (const auto& a : args) if (a.sampler || a.type.array) fail("invalid constructor argument", line);
        const std::string name = target.msl();
        if (target.is_scalar()) {
            const Expr& a = args[0];
            std::string src = a.text;
            if (a.type.is_vector()) src = swizzle(a.text, 1);
            else if (a.type.is_matrix()) src = "(" + a.text + ")[0][0]";
            return make(name + "(" + src + ")", target);
        }
        if (target.is_vector()) {
            const unsigned n = target.rows;
            if (args.size() == 1 && args[0].type.is_scalar()) {
                Expr a = args[0];
                if (a.type.base != target.base) a = Expr{name.substr(0, name.size() - 1) + "(" + a.text + ")", Type{target.base, 1, 1, 0}, false, {}};
                return make(name + "(" + a.text + ")", target);
            }
            std::string list;
            unsigned need = n;
            for (const auto& a : args) {
                if (need == 0) break;  // extra arguments are ignored, as drivers do
                if (a.type.is_matrix()) fail("cannot build a vector from a matrix", line);
                const unsigned have = a.type.components();
                const unsigned take = std::min(have, need);
                std::string piece = take == have ? a.text : swizzle(a.text, take);
                if (a.type.base != target.base) {
                    const Type cast{target.base, std::uint8_t(take), 1, 0};
                    piece = cast.msl() + "(" + piece + ")";
                }
                list += (list.empty() ? "" : ", ") + piece;
                need -= take;
            }
            if (need != 0) fail("too few components for " + target.glsl(), line);
            return make(name + "(" + list + ")", target);
        }
        // matrix
        const unsigned n = target.cols;
        if (args.size() == 1 && args[0].type.is_scalar()) return make("_glsl_mat" + std::to_string(n) + "(" + to_float(args[0]).text + ")", target);
        if (args.size() == 1 && args[0].type.is_matrix()) {
            if (args[0].type.cols != n) fail("matrix size conversion is not supported", line);
            return make(args[0].text, target);
        }
        bool all_columns = args.size() == n;
        for (const auto& a : args) all_columns = all_columns && a.type.is_vector() && a.type.rows == n;
        if (all_columns) {
            std::string list;
            for (const auto& a : args) list += (list.empty() ? "" : ", ") + to_float(a).text;
            return make(name + "(" + list + ")", target);
        }
        bool all_scalars = args.size() == n * n;
        for (const auto& a : args) all_scalars = all_scalars && a.type.is_scalar();
        if (all_scalars) {
            std::string list;
            for (unsigned c = 0; c < n; ++c) {
                std::string col = "float" + std::to_string(n) + "(";
                for (unsigned r = 0; r < n; ++r) col += (r ? ", " : "") + to_float(args[c * n + r]).text;
                list += (c ? ", " : "") + col + ")";
            }
            return make(name + "(" + list + ")", target);
        }
        fail("unsupported matrix constructor arguments", line);
    }

    // ---- calls
    Expr call(const std::string& name, std::vector<Expr> args, int line) {
        if (const auto it = fns_.find(name); it != fns_.end()) {
            const FnSig* best = nullptr;
            for (const auto& sig : it->second) {
                if (sig.params.size() != args.size()) continue;
                bool match = true;
                for (std::size_t i = 0; i < args.size(); ++i) match = match && (sig.params[i].components() == args[i].type.components() || args[i].type.array);
                if (match) best = &sig;
            }
            if (!best) fail("no matching overload of '" + name + "'", line);
            std::string text = best->name + "(";
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (args[i].sampler) fail("sampler arguments are not supported", line);
                text += convert(args[i], best->params[i]).text + ", ";
            }
            text += hidden_args(best->hidden_samplers) + ")";
            return make(text, best->ret);
        }
        return builtin(name, std::move(args), line);
    }
    Expr builtin(const std::string& name, std::vector<Expr> args, int line) {
        const auto want = [&](std::size_t n) { if (args.size() != n) fail("'" + name + "' takes " + std::to_string(n) + " arguments", line); };
        const Type f1{Base::float_, 1, 1, 0}, b1{Base::bool_, 1, 1, 0};
        if (name == "texture2D" || name == "texture2DProj" || name == "texture2DLod") {
            if (stage_ != Stage::fragment) fail("'" + name + "' is only supported in fragment shaders", line);
            const bool lod = name == "texture2DLod";
            if (args.size() < 2 || args.size() > 3 || (lod && args.size() != 3)) fail("wrong argument count for '" + name + "'", line);
            if (!args[0].sampler) fail("first argument of '" + name + "' must be a sampler2D", line);
            const auto n = safe(args[0].sampler_name);
            std::string coord = args[1].text;
            if (name == "texture2DProj") {
                if (args[1].type.rows == 3 && args[1].type.is_vector()) coord = "(" + swizzle(args[1].text, 2) + " / " + args[1].text + ".z)";
                else if (args[1].type.rows == 4 && args[1].type.is_vector()) coord = "(" + swizzle(args[1].text, 2) + " / " + args[1].text + ".w)";
                else fail("texture2DProj needs a vec3 or vec4", line);
            } else if (!(args[1].type.is_vector() && args[1].type.rows == 2)) {
                fail("texture2D needs a vec2 coordinate", line);
            }
            std::string extra;
            if (args.size() == 3) extra = std::string(", ") + (lod ? "level(" : "bias(") + to_float(args[2]).text + ")";
            return make("t_" + n + ".sample(m_" + n + ", " + coord + extra + ")", Type{Base::float_, 4, 1, 0});
        }
        for (const auto& a : args) if (a.sampler) fail("a sampler can only be used as a texture2D argument", line);

        // scalar-returning reductions
        if (name == "length" || name == "dot" || name == "distance") {
            want(name == "length" ? 1 : 2);
            for (auto& a : args) a = to_float(std::move(a));
            return make(name + "(" + join(args) + ")", f1);
        }
        if (name == "cross") { want(2); return make("cross(" + join(args) + ")", Type{Base::float_, 3, 1, 0}); }
        if (name == "any" || name == "all") { want(1); return make(name + "(" + args[0].text + ")", b1); }
        if (name == "not") { want(1); return make("(!" + args[0].text + ")", args[0].type); }
        static const std::map<std::string, std::string> relational = {{"lessThan", "<"}, {"lessThanEqual", "<="}, {"greaterThan", ">"},
                                                                       {"greaterThanEqual", ">="}, {"equal", "=="}, {"notEqual", "!="}};
        if (const auto r = relational.find(name); r != relational.end()) {
            want(2);
            if (!args[0].type.is_vector()) fail("'" + name + "' needs vectors", line);
            if (args[0].type.base != args[1].type.base) { args[0] = to_float(std::move(args[0])); args[1] = to_float(std::move(args[1])); }
            return make("(" + args[0].text + " " + r->second + " " + args[1].text + ")", vec_of(Base::bool_, args[0].type.rows));
        }
        if (name == "radians" || name == "degrees") {
            want(1);
            args[0] = to_float(std::move(args[0]));
            return make("(" + args[0].text + " * " + (name == "radians" ? "0.017453292519943295f" : "57.29577951308232f") + ")", args[0].type);
        }
        if (name == "mod") {
            want(2);
            for (auto& a : args) a = to_float(std::move(a));
            return make("_glsl_mod(" + join(args) + ")", args[0].type);
        }
        static const std::unordered_set<std::string> unary = {"abs", "sign", "floor", "ceil", "fract", "sqrt", "inversesqrt", "exp", "exp2",
                                                              "log", "log2", "sin", "cos", "tan", "asin", "acos", "normalize"};
        if (unary.contains(name) || (name == "atan" && args.size() == 1)) {
            want(1);
            args[0] = to_float(std::move(args[0]));
            return make((name == "inversesqrt" ? "rsqrt" : name) + "(" + args[0].text + ")", args[0].type);
        }
        static const std::unordered_set<std::string> multi = {"pow", "min", "max", "clamp", "mix", "step", "smoothstep", "atan", "reflect", "refract", "faceforward"};
        if (multi.contains(name)) {
            const bool refract = name == "refract";
            Type widest = f1;
            for (std::size_t i = 0; i < args.size(); ++i) {
                if (refract && i == 2) continue;
                if (args[i].type.is_vector() && args[i].type.components() > widest.components()) widest = args[i].type;
            }
            std::string list;
            for (std::size_t i = 0; i < args.size(); ++i) {
                Expr a = to_float(args[i]);
                if (widest.is_vector() && a.type.is_scalar() && !(refract && i == 2)) a.text = widest.msl() + "(" + a.text + ")";
                list += (i ? ", " : "") + a.text;
            }
            Type result = widest;
            if (name == "step" && args.size() == 2) result = widest;
            return make((name == "atan" ? "atan2" : name) + "(" + list + ")", result);
        }
        fail("unknown function '" + name + "'", line);
    }
    static std::string join(const std::vector<Expr>& args) {
        std::string s;
        for (std::size_t i = 0; i < args.size(); ++i) s += (i ? ", " : "") + args[i].text;
        return s;
    }

    Stage stage_;
    std::vector<Tok> toks_;
    std::size_t pos_{};
    ShaderUnit* unit_{};
    std::vector<std::unordered_map<std::string, Sym>> scopes_;
    std::unordered_map<std::string, std::vector<FnSig>> fns_;
    bool has_main_{};
    bool in_main_{};
};
}

ShaderUnit compile(Stage stage, std::string_view source) {
    ShaderUnit unit;
    unit.stage = stage;
    try {
        // Shader sources reach us with a trailing NUL when copied from fixed buffers.
        while (!source.empty() && source.back() == '\0') source.remove_suffix(1);
        Parser parser(stage, Lexer(source).run());
        parser.run(unit);
        unit.ok = true;
    } catch (const CompileError& e) {
        unit.ok = false;
        unit.log = "ERROR: 0:" + std::to_string(e.line) + ": " + e.message + "\n";
    }
    return unit;
}

std::uint32_t Linked::uniform_location(std::string_view name) const {
    std::string base(name);
    std::uint32_t element = 0;
    if (base.ends_with("]")) {
        const auto open = base.rfind('[');
        if (open == std::string::npos) return ~0u;
        char* end = nullptr;
        element = static_cast<std::uint32_t>(std::strtoul(base.c_str() + open + 1, &end, 10));
        if (end == base.c_str() + open + 1 || *end != ']') return ~0u;
        base.resize(open);
    }
    for (const auto& u : uniforms) if (u.name == base) return element < u.count() ? u.location + element : ~0u;
    for (std::size_t i = 0; i < samplers.size(); ++i) if (samplers[i].name == base && element == 0) return sampler_location_base + static_cast<std::uint32_t>(i);
    return ~0u;
}
std::uint32_t Linked::attribute_location(std::string_view name) const {
    for (std::size_t i = 0; i < attributes.size(); ++i) if (attributes[i].name == name) return static_cast<std::uint32_t>(i);
    return ~0u;
}

namespace {
const char* prelude = R"(#include <metal_stdlib>
using namespace metal;
inline float2x2 _glsl_mat2(float s) { return float2x2(float2(s, 0.0f), float2(0.0f, s)); }
inline float3x3 _glsl_mat3(float s) { return float3x3(float3(s, 0.0f, 0.0f), float3(0.0f, s, 0.0f), float3(0.0f, 0.0f, s)); }
inline float4x4 _glsl_mat4(float s) { return float4x4(float4(s, 0.0f, 0.0f, 0.0f), float4(0.0f, s, 0.0f, 0.0f), float4(0.0f, 0.0f, s, 0.0f), float4(0.0f, 0.0f, 0.0f, s)); }
inline bool _glsl_meq(float2x2 a, float2x2 b) { return all(a[0] == b[0]) && all(a[1] == b[1]); }
inline bool _glsl_meq(float3x3 a, float3x3 b) { return all(a[0] == b[0]) && all(a[1] == b[1]) && all(a[2] == b[2]); }
inline bool _glsl_meq(float4x4 a, float4x4 b) { return all(a[0] == b[0]) && all(a[1] == b[1]) && all(a[2] == b[2]) && all(a[3] == b[3]); }
template <typename T, typename U> inline T _glsl_mod(T x, U y) { return x - y * floor(x / y); }
)";
}

Linked link(const ShaderUnit& vs, const ShaderUnit& fs) {
    Linked out;
    if (!vs.ok || !fs.ok || vs.stage != Stage::vertex || fs.stage != Stage::fragment) {
        out.log = "ERROR: link needs a successfully compiled vertex and fragment shader\n";
        return out;
    }
    const auto merge = [&](std::vector<Declaration>& into, const std::vector<Declaration>& from, const char* what) -> bool {
        for (const auto& d : from) {
            const auto it = std::find_if(into.begin(), into.end(), [&](const Declaration& e) { return e.name == d.name; });
            if (it == into.end()) into.push_back(d);
            else if (!(it->type == d.type)) { out.log = std::string("ERROR: ") + what + " '" + d.name + "' is declared with different types\n"; return false; }
        }
        return true;
    };
    std::vector<Declaration> uniforms, varyings;
    if (!merge(uniforms, vs.uniforms, "uniform") || !merge(uniforms, fs.uniforms, "uniform")) return out;
    if (!merge(varyings, vs.varyings, "varying") || !merge(varyings, fs.varyings, "varying")) return out;
    for (const auto& f : fs.varyings) {
        if (std::none_of(vs.varyings.begin(), vs.varyings.end(), [&](const Declaration& d) { return d.name == f.name; })) {
            // GL leaves such a varying undefined; here it reads as zero because the vertex stage zero-initializes its outputs.
            out.log += "WARNING: varying '" + f.name + "' is read by the fragment shader but never declared by the vertex shader\n";
        }
    }
    out.attributes = vs.attributes;
    out.samplers = fs.samplers;

    // uniform block: every uniform starts on a 16-byte boundary so the host-side layout never depends on MSL padding rules
    std::string block = "struct Uniforms {\n";
    std::uint32_t offset = 0, location = 0;
    for (const auto& u : uniforms) {
        if (u.type.base == Base::sampler2d) continue;
        UniformSlot slot;
        slot.name = u.name;
        slot.type = u.type;
        slot.stride = msl_size(u.type);
        slot.offset = (offset + 15) & ~15u;
        slot.location = location;
        location += slot.count();
        const std::uint32_t end = slot.offset + slot.stride * slot.count();
        const std::uint32_t next = (end + 15) & ~15u;
        block += "    " + storage_msl(u.type) + " " + safe(u.name) + (u.type.array ? "[" + std::to_string(u.type.array) + "]" : "") + ";\n";
        if (next > end) block += "    float _pad" + std::to_string(out.uniforms.size()) + "[" + std::to_string((next - end) / 4) + "];\n";
        offset = next;
        out.uniforms.push_back(std::move(slot));
    }
    if (out.uniforms.empty()) { block += "    float _unused[4];\n"; offset = 16; }
    block += "};\n";
    out.uniform_bytes = offset;
    out.sampler_location_base = location;

    std::string vin = "struct VIn {\n";
    for (std::size_t i = 0; i < out.attributes.size(); ++i)
        vin += "    " + out.attributes[i].type.msl() + " " + safe(out.attributes[i].name) + " [[attribute(" + std::to_string(i) + ")]];\n";
    if (out.attributes.empty()) vin += "    int _unused;\n";
    vin += "};\n";
    std::string vout = "struct VOut {\n    float4 gl_Position [[position]];\n";
    for (const auto& v : varyings) vout += "    " + v.type.msl() + " " + safe(v.name) + ";\n";
    vout += "};\nstruct FOut {\n    float4 gl_FragColor [[color(0)]];\n};\n";

    std::string text = prelude;
    text += block + vin + vout + vs.functions + fs.functions;
    text += out.attributes.empty()
        ? "vertex VOut vs_main(constant Uniforms& u [[buffer(0)]]) {\n    VIn a = VIn();\n"
        : "vertex VOut vs_main(VIn a [[stage_in]], constant Uniforms& u [[buffer(0)]]) {\n";
    text += "    VOut o = VOut();\n    vs_user_main(u, a, o);\n"
            "    o.gl_Position.z = (o.gl_Position.z + o.gl_Position.w) * 0.5f;\n    return o;\n}\n";
    text += "fragment FOut fs_main(VOut v [[stage_in]], constant Uniforms& u [[buffer(0)]]";
    std::string call = "u, v, f";
    for (std::size_t i = 0; i < out.samplers.size(); ++i) {
        const auto n = safe(out.samplers[i].name);
        text += ", texture2d<float> t_" + n + " [[texture(" + std::to_string(i) + ")]], sampler m_" + n + " [[sampler(" + std::to_string(i) + ")]]";
        call += ", t_" + n + ", m_" + n;
    }
    text += ") {\n    FOut f = FOut();\n    fs_user_main(" + call + ");\n    return f;\n}\n";
    out.msl = std::move(text);
    out.ok = true;
    return out;
}
}
