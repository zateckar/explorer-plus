#include "syntax.h"
#include <algorithm>
#include <cwctype>
#include <cstring>
#include <memory>
#include <string_view>
#include <unordered_set>

namespace {

const size_t npos = std::wstring::npos;

// the coloured pieces, at `base` in the caller's text
struct Out {
    std::vector<SyntaxSpan>* v;
    size_t base = 0;
    void operator()(size_t a, size_t b, Tok t) const {
        if (v && b > a && t != Tok::Plain) v->push_back({ (uint32_t)(base + a), (uint32_t)(b - a), t });
    }
};

bool eol(wchar_t c) { return c == L'\r' || c == L'\n'; }
bool space(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\f' || c == L'\v' || c == 0xA0; }
bool digit(wchar_t c) { return c >= L'0' && c <= L'9'; }
bool hexdigit(wchar_t c) { return digit(c) || ((c | 0x20) >= L'a' && (c | 0x20) <= L'f'); }
bool alpha(wchar_t c) { return ((c | 0x20) >= L'a' && (c | 0x20) <= L'z') || c == L'_' || (c >= 0x80 && iswalpha(c)); }
bool alnum(wchar_t c) { return alpha(c) || digit(c); }
bool has_char(const wchar_t* set, wchar_t c) { return c && wcschr(set, c); }

size_t line_end(const wchar_t* s, size_t n, size_t i) {
    while (i < n && !eol(s[i])) ++i;
    return i;
}

size_t skip_space(const wchar_t* s, size_t n, size_t i) {
    while (i < n && space(s[i])) ++i;
    return i;
}

bool at(const wchar_t* s, size_t n, size_t i, const wchar_t* w) {
    for (; *w; ++w, ++i)
        if (i >= n || s[i] != *w) return false;
    return true;
}

bool at_nocase(const wchar_t* s, size_t n, size_t i, const wchar_t* w) {
    for (; *w; ++w, ++i)
        if (i >= n || towlower(s[i]) != *w) return false;
    return true;
}

size_t find(const wchar_t* s, size_t n, size_t from, const wchar_t* w) {
    size_t len = wcslen(w);
    for (size_t i = from; i + len <= n; ++i)
        if (s[i] == w[0] && at(s, n, i, w)) return i;
    return npos;
}

// a set of words, from a space-separated list (lists of languages without case: in lower case)
struct Words {
    std::unordered_set<std::wstring_view> set;
    bool noCase = false;
    Words(const wchar_t* list, bool nc) : noCase(nc) {
        for (const wchar_t* p = list; *p;) {
            while (*p == L' ') ++p;
            const wchar_t* b = p;
            while (*p && *p != L' ') ++p;
            if (p > b) set.insert(std::wstring_view(b, (size_t)(p - b)));
        }
    }
    bool has(std::wstring_view w) const {
        if (set.empty()) return false;
        if (!noCase) return set.count(w) != 0;
        wchar_t b[64];
        if (w.size() >= 64) return false;
        for (size_t i = 0; i < w.size(); ++i) b[i] = (wchar_t)towlower(w[i]);
        return set.count(std::wstring_view(b, w.size())) != 0;
    }
};

// ------------------------------------------------------------------
// languages like C: one lexer, set up per language
// ------------------------------------------------------------------
struct Spec {
    const wchar_t* line1 = nullptr;             // line comments
    const wchar_t* line2 = nullptr;
    const wchar_t* blockOpen = nullptr;         // block comments
    const wchar_t* blockClose = nullptr;
    const wchar_t* quotes = L"\"'";
    const wchar_t* multi = L"";                 // quotes whose strings go on across lines
    wchar_t escape = L'\\';                     // (0: none)
    bool doubled = false;                       // a quote doubled is one in the string ('it''s')
    bool triple = false;                        // """ and ''' strings, across lines
    bool verbatim = false;                      // C#'s @"...": across lines, no escapes
    bool hashPreproc = false;                   // # starting a line: a directive
    bool atNames = false;                       // @name: a decorator, an annotation
    bool dollar = false;                        // $name, ${name}: a variable
    bool hashNeedsSpace = false;                // (shell: # starts a comment only after a space)
    bool noCase = false;                        // keywords in any case
    bool keyStrings = false;                    // (JSON) a string before ':' is a key
    bool dashWords = false;                     // (PowerShell) Verb-Noun, -eq, -Path
    bool rustChars = false;                     // ' only as a character ('a', '\n'), else a lifetime
    const wchar_t* idExtra = L"";               // name characters besides letters, digits and _
    const wchar_t* keywords = L"";
    const wchar_t* types = L"";
    const wchar_t* literals = L"";
};

const Spec kC = [] {
    Spec s;
    s.line1 = L"//"; s.blockOpen = L"/*"; s.blockClose = L"*/"; s.hashPreproc = true;
    s.keywords = L"alignas alignof asm auto break case catch class concept const consteval constexpr constinit const_cast "
                 L"continue co_await co_return co_yield decltype default delete do dynamic_cast else enum explicit export "
                 L"extern final for friend goto if import inline module mutable namespace new noexcept operator override "
                 L"private protected public register reinterpret_cast requires return sizeof static static_assert "
                 L"static_cast struct switch template this thread_local throw try typedef typeid typename union using "
                 L"virtual volatile while __declspec __stdcall __cdecl";
    s.types = L"bool char char8_t char16_t char32_t double float int long short signed unsigned void wchar_t size_t "
              L"ptrdiff_t int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t intptr_t uintptr_t "
              L"BOOL BYTE WORD DWORD UINT LONG ULONG HANDLE HWND HRESULT LPARAM WPARAM LRESULT WCHAR";
    s.literals = L"true false nullptr NULL TRUE FALSE";
    return s;
}();

const Spec kCSharp = [] {
    Spec s;
    s.line1 = L"//"; s.blockOpen = L"/*"; s.blockClose = L"*/"; s.hashPreproc = true; s.verbatim = true; s.triple = true;
    s.atNames = false;
    s.keywords = L"abstract as async await base break case catch checked class const continue default delegate do "
                 L"dynamic else enum event explicit extern finally fixed for foreach get global goto if implicit in init "
                 L"interface internal is lock nameof namespace new operator out override params partial private protected "
                 L"public readonly record ref required return sealed set sizeof stackalloc static struct switch this throw "
                 L"try typeof unchecked unsafe using value var virtual volatile when where while with yield";
    s.types = L"bool byte char decimal double float int long nint nuint object sbyte short string uint ulong ushort void";
    s.literals = L"true false null";
    return s;
}();

const Spec kJava = [] {
    Spec s;
    s.line1 = L"//"; s.blockOpen = L"/*"; s.blockClose = L"*/"; s.atNames = true; s.triple = true;
    s.keywords = L"abstract assert break by case catch class companion const continue data default do else enum extends "
                 L"final finally for fun goto if implements import in init instanceof interface internal is lateinit "
                 L"native new object open override package permits private protected public record return sealed static "
                 L"strictfp super suspend switch synchronized this throw throws transient try typealias val var volatile "
                 L"when while yield def";
    s.types = L"boolean byte char double float int long short void String Integer Long Double Boolean Object Unit Int Any";
    s.literals = L"true false null";
    return s;
}();

const wchar_t* const kJsKeywords =
    L"async await break case catch class const continue debugger default delete do else export extends finally for from "
    L"function get if import in instanceof let new of return set static super switch this throw try typeof var void while "
    L"with yield";

const Spec kJavaScript = [] {
    Spec s;
    s.line1 = L"//"; s.blockOpen = L"/*"; s.blockClose = L"*/"; s.quotes = L"\"'`"; s.multi = L"`"; s.idExtra = L"$";
    s.keywords = kJsKeywords;
    s.literals = L"true false null undefined NaN Infinity";
    return s;
}();

const Spec kTypeScript = [] {
    Spec s = kJavaScript;
    s.atNames = true;
    static const std::wstring kw = std::wstring(kJsKeywords) +
        L" abstract as declare enum implements infer interface is keyof module namespace override private protected public "
        L"readonly satisfies type";
    s.keywords = kw.c_str();
    s.types = L"any bigint boolean never number object string symbol unknown void";
    return s;
}();

const Spec kGo = [] {
    Spec s;
    s.line1 = L"//"; s.blockOpen = L"/*"; s.blockClose = L"*/"; s.quotes = L"\"'`"; s.multi = L"`";
    s.keywords = L"break case chan const continue default defer else fallthrough for func go goto if import interface map "
                 L"package range return select struct switch type var";
    s.types = L"any bool byte complex64 complex128 error float32 float64 int int8 int16 int32 int64 rune string uint uint8 "
              L"uint16 uint32 uint64 uintptr";
    s.literals = L"true false nil iota";
    return s;
}();

const Spec kRust = [] {
    Spec s;
    s.line1 = L"//"; s.blockOpen = L"/*"; s.blockClose = L"*/"; s.rustChars = true; s.hashPreproc = true;
    s.multi = L"\"";
    s.keywords = L"as async await break const continue crate dyn else enum extern fn for if impl in let loop match mod move "
                 L"mut pub ref return self static struct super trait type unsafe use where while macro_rules";
    s.types = L"bool char f32 f64 i8 i16 i32 i64 i128 isize str u8 u16 u32 u64 u128 usize String Vec Option Result Box Self";
    s.literals = L"true false None Some Ok Err";
    return s;
}();

const Spec kPython = [] {
    Spec s;
    s.line1 = L"#"; s.triple = true; s.atNames = true;
    s.keywords = L"and as assert async await break case class continue def del elif else except finally for from global if "
                 L"import in is lambda match nonlocal not or pass raise return try while with yield";
    s.types = L"bool bytearray bytes complex dict float frozenset int list object set str tuple type";
    s.literals = L"True False None self cls";
    return s;
}();

const Spec kRuby = [] {
    Spec s;
    s.line1 = L"#"; s.quotes = L"\"'`"; s.atNames = true; s.dollar = true;
    s.keywords = L"alias and begin break case class def defined do else elsif end ensure for if in module next not or redo "
                 L"rescue retry return super then undef unless until when while yield require require_relative "
                 L"attr_accessor attr_reader attr_writer private protected public";
    s.literals = L"true false nil self";
    return s;
}();

const Spec kLua = [] {
    Spec s;
    s.line1 = L"--"; s.blockOpen = L"--[["; s.blockClose = L"]]";
    s.keywords = L"and break do else elseif end for function goto if in local not or repeat return then until while";
    s.literals = L"true false nil self";
    return s;
}();

const Spec kPowerShell = [] {
    Spec s;
    s.line1 = L"#"; s.blockOpen = L"<#"; s.blockClose = L"#>"; s.multi = L"\"'"; s.escape = L'`'; s.doubled = true;
    s.dollar = true; s.noCase = true; s.dashWords = true; s.atNames = false;
    s.keywords = L"begin break catch class continue data define do dynamicparam else elseif end enum exit filter finally "
                 L"for foreach from function hidden if in param process return static switch throw trap try until using "
                 L"var while workflow -and -or -not -xor -band -bor -bxor -bnot -eq -ne -gt -ge -lt -le -like -notlike "
                 L"-match -notmatch -contains -notcontains -in -notin -replace -split -join -is -isnot -as -f -ceq -cne "
                 L"-cgt -clt -clike -cmatch -cnotmatch -creplace -csplit -ieq -ine -ilike -imatch -ireplace -shl -shr";
    s.types = L"string int long bool byte char double decimal float array hashtable object void datetime pscustomobject "
              L"scriptblock regex xml switch psobject";
    s.literals = L"$true $false $null";
    return s;
}();

const Spec kShell = [] {
    Spec s;
    s.line1 = L"#"; s.hashNeedsSpace = true; s.quotes = L"\"'`"; s.multi = L"\"'`"; s.dollar = true;
    s.keywords = L"if then else elif fi case esac for select while until do done in function time coproc return exit break "
                 L"continue local export readonly declare typeset unset shift source alias eval exec set trap echo printf "
                 L"cd test let";
    s.literals = L"true false";
    return s;
}();

const Spec kSql = [] {
    Spec s;
    s.line1 = L"--"; s.blockOpen = L"/*"; s.blockClose = L"*/"; s.quotes = L"'\"`"; s.multi = L"'"; s.escape = 0;
    s.doubled = true; s.noCase = true; s.atNames = true;
    s.keywords = L"add all alter and any as asc authorization backup begin between break by cascade case cast check "
                 L"checkpoint close clustered coalesce collate column commit constraint contains continue convert create "
                 L"cross current current_date current_time current_timestamp current_user cursor database deallocate "
                 L"declare default delete deny desc distinct drop else end escape except exec execute exists exit fetch "
                 L"following for foreign from full function go goto grant group having identity if ilike in index inner "
                 L"insert intersect into is join key kill left like limit materialized merge natural nocheck nonclustered "
                 L"not nullif of off offset on open option or order outer over partition percent pivot plan preceding "
                 L"primary print proc procedure raiserror range read recursive references rename replace restrict return "
                 L"returning returns revoke right rollback rows rule save schema select set some table temp temporary then "
                 L"to top tran transaction trigger truncate unbounded union unique unpivot update use user using values "
                 L"view when where while window with";
    s.types = L"bigint binary bit blob boolean char clob date datetime datetime2 datetimeoffset decimal double float int "
              L"integer interval json jsonb money nchar ntext numeric nvarchar real serial smallint text time timestamp "
              L"tinyint uniqueidentifier uuid varbinary varchar xml";
    s.literals = L"true false null";
    return s;
}();

const Spec kJson = [] {
    Spec s;
    s.line1 = L"//"; s.blockOpen = L"/*"; s.blockClose = L"*/"; s.keyStrings = true;
    s.literals = L"true false null NaN Infinity";
    return s;
}();

// a string's prefix letters (r"", b'', f"", L"", u8"", N'')
bool string_prefix(const wchar_t* s, size_t len) {
    if (len > 2) return false;
    for (size_t i = 0; i < len; ++i)
        if (!has_char(L"rRbBfFuUL8nN", s[i])) return false;
    return true;
}

// past a string's end from j (after its opening quotes); `closed`: its closing quote found. Kinds:
// 0 on one line (ends at the line's end), 2 across lines, 3 """, 4 verbatim (@"", no escapes).
size_t scan_string(const Spec& sp, const wchar_t* s, size_t n, size_t j, wchar_t q, int kind, bool& closed) {
    closed = false;
    while (j < n) {
        wchar_t c = s[j];
        if (kind == 4) {
            if (c == q) {
                if (j + 1 < n && s[j + 1] == q) { j += 2; continue; }
                closed = true;
                return j + 1;
            }
            ++j;
            continue;
        }
        if (sp.escape && c == sp.escape) { j += 2; continue; }
        if (c == q) {
            if (kind == 3) {
                if (j + 2 < n && s[j + 1] == q && s[j + 2] == q) { closed = true; return j + 3; }
                ++j;
                continue;
            }
            if (sp.doubled && j + 1 < n && s[j + 1] == q) { j += 2; continue; }
            closed = true;
            return j + 1;
        }
        if (kind == 0 && eol(c)) return j;
        ++j;
    }
    return n;
}

// Rust: 'a', '\n', '\u{1F600}' - or a lifetime ('a): 0
size_t rust_char(const wchar_t* s, size_t n, size_t i) {
    if (i + 2 < n && s[i + 1] != L'\\' && s[i + 2] == L'\'') return i + 3;
    if (i + 1 < n && s[i + 1] == L'\\')
        for (size_t j = i + 2; j < n && j < i + 12 && !eol(s[j]); ++j)
            if (s[j] == L'\'') return j + 1;
    return 0;
}

struct Compiled {
    const Spec* sp;
    Words kw, ty, li;
    explicit Compiled(const Spec* s) : sp(s), kw(s->keywords, s->noCase), ty(s->types, s->noCase), li(s->literals, s->noCase) {}
};

// states: 0, 1 in a block comment, kind << 8 | quote in a string going on across lines
int lex_generic(const Compiled& cs, const wchar_t* s, size_t n, int state, Out out) {
    const Spec& sp = *cs.sp;
    size_t i = 0;
    if (state == 1 && sp.blockClose) {
        size_t e = find(s, n, 0, sp.blockClose);
        if (e == npos) { out(0, n, Tok::Comment); return 1; }
        i = e + wcslen(sp.blockClose);
        out(0, i, Tok::Comment);
    } else if (state >> 8) {
        bool closed = false;
        size_t e = scan_string(sp, s, n, 0, (wchar_t)(state & 0xFF), state >> 8, closed);
        out(0, e, Tok::String);
        if (!closed) return state;
        i = e;
    }
    bool lineStart = state == 0;
    // a string from `start`, its quote at q: false when it goes on past the text (*st: the state then)
    auto string_at = [&](size_t start, size_t q, int kind, int* st) {
        wchar_t c = s[q];
        size_t j = q + 1;
        if (kind == 0) {
            if (has_char(sp.multi, c)) kind = 2;
            if (sp.triple && q + 2 < n && s[q + 1] == c && s[q + 2] == c) { kind = 3; j = q + 3; }
        }
        bool closed = false;
        size_t e = scan_string(sp, s, n, j, c, kind, closed);
        Tok t = Tok::String;
        if (sp.keyStrings && closed) {
            size_t k = skip_space(s, n, e);
            if (k < n && s[k] == L':') t = Tok::Key;
        }
        out(start, e, t);
        i = e;
        if (!closed && kind) { *st = kind << 8 | (c & 0xFF); return false; }
        return true;
    };
    while (i < n) {
        wchar_t c = s[i];
        if (eol(c)) { lineStart = true; ++i; continue; }
        if (space(c)) { ++i; continue; }
        bool first = lineStart;
        lineStart = false;
        // comments, directives
        if (sp.blockOpen && c == sp.blockOpen[0] && at(s, n, i, sp.blockOpen)) {
            size_t e = find(s, n, i + wcslen(sp.blockOpen), sp.blockClose);
            if (e == npos) { out(i, n, Tok::Comment); return 1; }
            e += wcslen(sp.blockClose);
            out(i, e, Tok::Comment);
            i = e;
            continue;
        }
        if ((sp.line1 && c == sp.line1[0] && at(s, n, i, sp.line1)) || (sp.line2 && c == sp.line2[0] && at(s, n, i, sp.line2))) {
            if (!sp.hashNeedsSpace || i == 0 || space(s[i - 1]) || eol(s[i - 1])) {
                size_t e = line_end(s, n, i);
                out(i, e, Tok::Comment);
                i = e;
                continue;
            }
        }
        if (sp.hashPreproc && first && c == L'#') {
            size_t e = line_end(s, n, i);
            out(i, e, Tok::Preproc);
            i = e;
            continue;
        }
        // strings
        if (sp.rustChars && c == L'\'') {
            size_t e = rust_char(s, n, i);
            if (e) {
                out(i, e, Tok::String);
            } else {
                e = i + 1;
                while (e < n && alnum(s[e])) ++e;
                out(i, e, Tok::Variable);
            }
            i = e;
            continue;
        }
        if (has_char(sp.quotes, c)) {
            int st = 0;
            if (!string_at(i, i, 0, &st)) return st;
            continue;
        }
        if (sp.verbatim && (c == L'@' || c == L'$')) {
            size_t j = i;
            bool v = false;
            while (j < n && j < i + 3 && (s[j] == L'@' || s[j] == L'$')) v |= s[j++] == L'@';
            if (j < n && s[j] == L'"') {
                int st = 0;
                if (!string_at(i, j, v ? 4 : 0, &st)) return st;
                continue;
            }
        }
        // numbers
        if (digit(c) || (c == L'.' && i + 1 < n && digit(s[i + 1]))) {
            bool hex = c == L'0' && i + 1 < n && (s[i + 1] | 0x20) == L'x';
            size_t j = i + 1;
            while (j < n) {
                wchar_t d = s[j];
                if (alnum(d)) ++j;
                else if (d == L'.' && j + 1 < n && digit(s[j + 1])) ++j;
                else if ((d == L'+' || d == L'-') && !hex && (s[j - 1] | 0x20) == L'e') ++j;
                else break;
            }
            out(i, j, Tok::Number);
            i = j;
            continue;
        }
        if (sp.atNames && c == L'@' && i + 1 < n && alpha(s[i + 1])) {
            size_t j = i + 1;
            while (j < n && (alnum(s[j]) || s[j] == L'.')) ++j;
            out(i, j, Tok::Preproc);
            i = j;
            continue;
        }
        if (sp.dollar && c == L'$' && i + 1 < n) {
            size_t j = i + 1;
            if (s[j] == L'{') {
                while (j < n && s[j] != L'}' && !eol(s[j])) ++j;
                if (j < n && s[j] == L'}') ++j;
            } else if (s[j] == L'(') {
                j = i + 2;
            } else if (alpha(s[j])) {
                while (j < n && (alnum(s[j]) || (s[j] == L':' && j + 1 < n && alpha(s[j + 1]) && sp.dashWords))) ++j;
            } else if (digit(s[j]) || has_char(L"@#?$!*-_", s[j])) {
                ++j;
            }
            if (j > i + 1) {
                out(i, j, cs.li.has(std::wstring_view(s + i, j - i)) ? Tok::Literal : Tok::Variable);
                i = j;
                continue;
            }
        }
        // names: keywords, types, functions
        bool dashStart = sp.dashWords && c == L'-' && i + 1 < n && alpha(s[i + 1]) && (i == 0 || !alnum(s[i - 1]));
        if (alpha(c) || has_char(sp.idExtra, c) || dashStart) {
            size_t j = i + 1;
            while (j < n && (alnum(s[j]) || has_char(sp.idExtra, s[j]) || (sp.dashWords && s[j] == L'-' && j + 1 < n && alpha(s[j + 1]))))
                ++j;
            if (j < n && has_char(sp.quotes, s[j]) && string_prefix(s + i, j - i)) {
                int st = 0;
                if (!string_at(i, j, 0, &st)) return st;
                continue;
            }
            std::wstring_view w(s + i, j - i);
            Tok t = Tok::Plain;
            if (cs.li.has(w)) t = Tok::Literal;
            else if (cs.kw.has(w)) t = Tok::Keyword;
            else if (cs.ty.has(w)) t = Tok::Type;
            else if (dashStart) t = Tok::Attr;
            else {
                size_t k = skip_space(s, n, j);
                if (k < n && s[k] == L'(') t = Tok::Function;
                else if (sp.dashWords && w.find(L'-') != std::wstring_view::npos) t = Tok::Function;
            }
            out(i, j, t);
            i = j;
            continue;
        }
        ++i;
    }
    return 0;
}

// ------------------------------------------------------------------
// JSON-like value scanning shared by INI / TOML / YAML
// ------------------------------------------------------------------
bool number_text(const wchar_t* s, size_t a, size_t b) {
    if (a >= b) return false;
    size_t i = a;
    if (s[i] == L'+' || s[i] == L'-') ++i;
    if (i >= b) return false;
    if (b - i >= 2 && s[i] == L'0' && (s[i + 1] | 0x20) == L'x') {
        for (size_t k = i + 2; k < b; ++k)
            if (!hexdigit(s[k]) && s[k] != L'_') return false;
        return b - i > 2;
    }
    bool any = false;
    for (size_t k = i; k < b; ++k) {
        wchar_t c = s[k];
        if (digit(c)) any = true;
        else if (!(c == L'.' || c == L'_' || c == L':' || c == L'-' || c == L'+' || (c | 0x20) == L'e' || (c | 0x20) == L't' || c == L'Z'))
            return false;
    }
    return any && digit(s[i == b ? a : i]);
}

bool word_in(const wchar_t* s, size_t a, size_t b, const wchar_t* list) {

    std::wstring w;
    for (size_t k = a; k < b; ++k) w += (wchar_t)towlower(s[k]);
    for (const wchar_t* p = list; *p;) {
        while (*p == L' ') ++p;
        const wchar_t* q = p;
        while (*p && *p != L' ') ++p;
        if (p > q && w.size() == (size_t)(p - q) && wcsncmp(q, w.c_str(), w.size()) == 0) return true;
    }
    return false;
}

// ------------------------------------------------------------------
// INI, TOML (and .reg, .properties)
// ------------------------------------------------------------------
// states: 0, 1 in a """ string, 2 in a ''' string (TOML)
int lex_ini(const wchar_t* s, size_t n, int state, bool toml, Out out) {
    size_t i = 0;
    auto value = [&](size_t v, size_t e) -> int {
        int depth = 0;
        while (v < e) {
            wchar_t c = s[v];
            if (space(c) || c == L',' || c == L'=') { ++v; continue; }
            if (c == L'[' || c == L'{') { ++depth; ++v; continue; }
            if (c == L']' || c == L'}') { --depth; ++v; continue; }
            if (c == L'#' && (toml || v == 0 || space(s[v - 1]))) { out(v, e, Tok::Comment); return 0; }
            if (c == L'"' || c == L'\'') {
                if (toml && v + 2 < n && s[v + 1] == c && s[v + 2] == c) {
                    size_t close = find(s, n, v + 3, c == L'"' ? L"\"\"\"" : L"'''");
                    if (close == npos) { out(v, n, Tok::String); return c == L'"' ? 1 : 2; }
                    out(v, close + 3, Tok::String);
                    if (close + 3 > e) return -(int)(close + 3);    // (went past this line: go on from there)
                    v = close + 3;
                    continue;
                }
                size_t j = v + 1;
                while (j < e && s[j] != c) j += (s[j] == L'\\' && c == L'"' && toml) ? 2 : 1;
                j = j < e ? j + 1 : e;
                size_t k = skip_space(s, e, j);
                out(v, j, depth > 0 && k < e && s[k] == L'=' ? Tok::Key : Tok::String);
                v = j;
                continue;
            }
            // a bare word: up to a space, a comma or a bracket (INI: the rest of the line is the value)
            size_t j = v;
            while (j < e && !space(s[j]) && !has_char(L",[]{}=", s[j]) && !(s[j] == L'#' && toml)) ++j;
            if (j == v) { ++v; continue; }
            size_t k = skip_space(s, e, j);
            if (depth > 0 && k < e && s[k] == L'=') out(v, j, Tok::Key);
            else if (number_text(s, v, j)) out(v, j, Tok::Number);
            else if (word_in(s, v, j, toml ? L"true false inf nan" : L"true false yes no on off null none"))
                out(v, j, Tok::Literal);
            v = j;
        }
        return 0;
    };
    if (state) {
        size_t close = find(s, n, 0, state == 1 ? L"\"\"\"" : L"'''");
        if (close == npos) { out(0, n, Tok::String); return state; }
        out(0, close + 3, Tok::String);
        i = close + 3;
        size_t e = line_end(s, n, i);
        int r = value(i, e);
        if (r > 0) return r;
        i = e;
    }
    while (i < n) {
        size_t e = line_end(s, n, i);
        size_t k = skip_space(s, e, i);
        if (k < e) {
            wchar_t c = s[k];
            if (c == L';' || c == L'#') {
                out(k, e, Tok::Comment);
            } else if (c == L'[') {
                size_t close = k;
                while (close < e && s[close] != L']') ++close;
                if (close < e && s[close + 1 < e ? close + 1 : close] == L']') ++close;     // ([[array of tables]])
                out(k, close < e ? close + 1 : e, Tok::Heading);
                if (close < e) value(close + 1, e);
            } else {
                // the key: up to = (or : in a .properties file), quoted in .reg and TOML
                size_t eq = k;
                if (s[k] == L'"' || s[k] == L'\'') {
                    eq = k + 1;
                    while (eq < e && s[eq] != s[k]) eq += s[eq] == L'\\' ? 2 : 1;
                    if (eq < e) ++eq;
                }
                while (eq < e && s[eq] != L'=' && (toml || s[eq] != L':')) ++eq;
                if (eq < e) {
                    size_t ke = eq;
                    while (ke > k && space(s[ke - 1])) --ke;
                    out(k, ke, Tok::Key);
                    int r = value(eq + 1, e);
                    if (r > 0) return r;
                    if (r < 0) { i = (size_t)-r; continue; }
                } else {
                    value(k, e);
                }
            }
        }
        i = e;
        if (i < n) ++i;
    }
    return 0;
}

// ------------------------------------------------------------------
// YAML
// ------------------------------------------------------------------
// states: 0, 1 << 16 | indent in a block scalar (| or >) under a key at that indent
size_t yaml_key_end(const wchar_t* s, size_t k, size_t e) {
    size_t j = k;
    if (j < e && (s[j] == L'"' || s[j] == L'\'')) {
        wchar_t q = s[j++];
        while (j < e && s[j] != q) j += (s[j] == L'\\' && q == L'"') ? 2 : 1;
        if (j >= e) return npos;
        j = skip_space(s, e, j + 1);
        return j < e && s[j] == L':' && (j + 1 == e || space(s[j + 1])) ? j : npos;
    }
    if (j < e && has_char(L"[{&*!|>%@`", s[j])) return npos;
    for (; j < e; ++j) {
        if (s[j] == L':' && (j + 1 == e || space(s[j + 1]))) return j;
        if (s[j] == L'#' && j > k && space(s[j - 1])) return npos;
    }
    return npos;
}

int lex_yaml(const wchar_t* s, size_t n, int state, Out out) {
    size_t i = 0;
    while (i < n) {
        size_t e = line_end(s, n, i);
        size_t k = skip_space(s, e, i);
        int indent = (int)(k - i);
        if (state) {
            if (k == e || indent > (state & 0xFFFF)) {
                out(k, e, Tok::String);
                i = e < n ? e + 1 : e;
                continue;
            }
            state = 0;
        }
        if (k == e) { i = e < n ? e + 1 : e; continue; }
        if (s[k] == L'#') {
            out(k, e, Tok::Comment);
        } else if (k == i && (at(s, e, k, L"---") || at(s, e, k, L"...")) && (k + 3 == e || space(s[k + 3]))) {
            out(k, e, Tok::Preproc);
        } else {
            while (k < e && s[k] == L'-' && (k + 1 == e || space(s[k + 1]))) {
                out(k, k + 1, Tok::Keyword);
                k = skip_space(s, e, k + 1);
            }
            int keyIndent = (int)(k - i);
            size_t v = k;
            size_t ke = yaml_key_end(s, k, e);
            if (ke != npos) {
                out(k, ke, Tok::Key);
                v = ke + 1;
            }
            // the value
            v = skip_space(s, e, v);
            if (v < e && (s[v] == L'|' || s[v] == L'>')) {
                size_t j = v + 1;
                while (j < e && (has_char(L"+-", s[j]) || digit(s[j]))) ++j;
                out(v, j, Tok::Preproc);
                j = skip_space(s, e, j);
                if (j < e && s[j] == L'#') out(j, e, Tok::Comment);
                state = 1 << 16 | keyIndent;
            } else {
                int depth = 0;
                while (v < e) {
                    wchar_t c = s[v];
                    if (space(c) || c == L',') { ++v; continue; }
                    if (c == L'[' || c == L'{') { ++depth; ++v; continue; }
                    if (c == L']' || c == L'}') { --depth; ++v; continue; }
                    if (c == L'#' && (v == 0 || space(s[v - 1]))) { out(v, e, Tok::Comment); break; }
                    if ((c == L'&' || c == L'*' || c == L'!') && v + 1 < e && !space(s[v + 1])) {
                        size_t j = v + 1;
                        while (j < e && !space(s[j]) && !has_char(L",[]{}", s[j])) ++j;
                        out(v, j, c == L'!' ? Tok::Type : Tok::Variable);
                        v = j;
                        continue;
                    }
                    if (c == L'"' || c == L'\'') {
                        size_t j = v + 1;
                        while (j < e && s[j] != c) j += (s[j] == L'\\' && c == L'"') ? 2 : 1;
                        j = j < e ? j + 1 : e;
                        size_t kk = skip_space(s, e, j);
                        out(v, j, depth > 0 && kk < e && s[kk] == L':' ? Tok::Key : Tok::String);
                        v = j;
                        continue;
                    }
                    // a plain scalar: to the line's end (in brackets: to a comma or a bracket), or a comment
                    size_t j = v;
                    while (j < e && !(s[j] == L'#' && space(s[j - 1])) && !(depth > 0 && has_char(L",[]{}", s[j])) &&
                           !(depth > 0 && s[j] == L':' && (j + 1 == e || space(s[j + 1]))))
                        ++j;
                    size_t end = j;
                    while (end > v && space(s[end - 1])) --end;
                    if (depth > 0 && j < e && s[j] == L':') { out(v, end, Tok::Key); v = j + 1; continue; }
                    if (number_text(s, v, end) || word_in(s, v, end, L".inf -.inf .nan")) out(v, end, Tok::Number);
                    else if (word_in(s, v, end, L"true false yes no on off null ~")) out(v, end, Tok::Literal);
                    else out(v, end, Tok::String);
                    v = j;
                }
            }
        }
        i = e < n ? e + 1 : e;
    }
    return state;
}

// ------------------------------------------------------------------
// Markdown
// ------------------------------------------------------------------
void md_inline(const wchar_t* s, size_t a, size_t e, Out out) {
    size_t i = a;
    while (i < e) {
        wchar_t c = s[i];
        if (c == L'\\') { i += 2; continue; }
        if (c == L'`') {
            size_t r = i;
            while (r < e && s[r] == L'`') ++r;
            size_t run = r - i, j = r;
            size_t close = npos;
            while (j < e) {
                if (s[j] == L'`') {
                    size_t q = j;
                    while (q < e && s[q] == L'`') ++q;
                    if (q - j == run) { close = j; break; }
                    j = q;
                } else {
                    ++j;
                }
            }
            if (close != npos) { out(i, close + run, Tok::String); i = close + run; }
            else i = r;
            continue;
        }
        if (c == L'*' || c == L'_') {
            size_t r = i;
            while (r < e && s[r] == c && r - i < 3) ++r;
            size_t run = r - i;
            if ((c == L'_' && i > a && alnum(s[i - 1])) || r >= e || space(s[r])) { i = r; continue; }
            size_t close = npos;
            for (size_t j = r + 1; j + run <= e; ++j) {
                bool ok = true;
                for (size_t k = 0; k < run; ++k)
                    if (s[j + k] != c) { ok = false; break; }
                if (ok && !space(s[j - 1]) && !(c == L'_' && j + run < e && alnum(s[j + run]))) { close = j; break; }
            }
            if (close != npos) { out(i, close + run, Tok::Emphasis); i = close + run; }
            else i = r;
            continue;
        }
        if (c == L'[' || (c == L'!' && i + 1 < e && s[i + 1] == L'[')) {
            size_t start = i, b = c == L'!' ? i + 1 : i;
            size_t close = b + 1;
            int depth = 1;
            for (; close < e; ++close) {
                if (s[close] == L'[') ++depth;
                else if (s[close] == L']' && --depth == 0) break;
            }
            if (close < e && close + 1 < e && s[close + 1] == L'(') {
                size_t p = close + 2;
                while (p < e && s[p] != L')') ++p;
                if (p < e) {
                    out(start, close + 1, Tok::Link);
                    out(close + 1, p + 1, Tok::String);
                    i = p + 1;
                    continue;
                }
            }
            if (close < e && close + 1 < e && (s[close + 1] == L'[' || s[close + 1] == L':')) {
                out(start, close + 1, Tok::Link);
                i = close + 1;
                continue;
            }
            i = b + 1;
            continue;
        }
        if (c == L'<' && (at(s, e, i + 1, L"http") || at(s, e, i + 1, L"mailto:"))) {
            size_t p = i + 1;
            while (p < e && s[p] != L'>' && !space(s[p])) ++p;
            if (p < e && s[p] == L'>') { out(i, p + 1, Tok::Link); i = p + 1; continue; }
        }
        if (c == L'h' && (i == a || !alnum(s[i - 1])) && (at(s, e, i, L"http://") || at(s, e, i, L"https://"))) {
            size_t p = i;
            while (p < e && !space(s[p]) && !has_char(L")>]\"'", s[p])) ++p;
            out(i, p, Tok::Link);
            i = p;
            continue;
        }
        ++i;
    }
}

bool md_rule(const wchar_t* s, size_t k, size_t e) {
    if (k >= e || !has_char(L"-*_=", s[k])) return false;
    wchar_t c = s[k];
    int count = 0;
    for (size_t j = k; j < e; ++j) {
        if (s[j] == c) ++count;
        else if (!space(s[j])) return false;
    }
    return count >= 3 || (c == L'=' && count >= 1);
}

// states: 0, 1 in a ``` block, 2 in a ~~~ block
int lex_markdown(const wchar_t* s, size_t n, int state, Out out) {
    size_t i = 0;
    while (i < n) {
        size_t e = line_end(s, n, i);
        size_t k = i;
        while (k < e && k - i < 3 && s[k] == L' ') ++k;
        wchar_t f = k + 2 < e && (s[k] == L'`' || s[k] == L'~') && s[k + 1] == s[k] && s[k + 2] == s[k] ? s[k] : 0;
        if (state) {
            if (f == (state == 1 ? L'`' : L'~')) { out(i, e, Tok::Preproc); state = 0; }
            else out(i, e, Tok::String);
        } else if (f) {
            out(k, e, Tok::Preproc);
            state = f == L'`' ? 1 : 2;
        } else if (k < e && s[k] == L'#') {
            size_t h = k;
            while (h < e && s[h] == L'#') ++h;
            if (h - k <= 6 && (h == e || space(s[h]))) out(k, e, Tok::Heading);
            else md_inline(s, k, e, out);
        } else if (k < e && s[k] == L'>') {
            out(k, e, Tok::Comment);
        } else if (md_rule(s, k, e)) {
            out(k, e, s[k] == L'=' ? Tok::Heading : Tok::Preproc);
        } else {
            size_t m = k;
            while (m < e && space(s[m])) ++m;
            if (m < e && has_char(L"-*+", s[m]) && (m + 1 == e || space(s[m + 1]))) {
                out(m, m + 1, Tok::Keyword);
                m += 1;
            } else {
                size_t d = m;
                while (d < e && digit(s[d])) ++d;
                if (d > m && d - m < 10 && d < e && (s[d] == L'.' || s[d] == L')') && (d + 1 == e || space(s[d + 1]))) {
                    out(m, d + 1, Tok::Keyword);
                    m = d + 1;
                }
            }
            md_inline(s, m, e, out);
        }
        i = e < n ? e + 1 : e;
    }
    return state;
}

// ------------------------------------------------------------------
// XML, HTML
// ------------------------------------------------------------------
bool name_char(wchar_t c) { return alnum(c) || c == L':' || c == L'-' || c == L'.'; }

const Compiled& compiled(Lang l);
int lex_css(const wchar_t* s, size_t n, int state, Out out);

// states: 0 text, 1 in a tag, 2 comment, 3 CDATA, 4 / 5 in a "- / '-quoted value, 6 <?...?>, 7 <!...>,
// 8 in <script>, 9 in <style> (HTML)
int lex_xml(const wchar_t* s, size_t n, int state, bool html, Out out) {
    size_t i = 0;
    std::wstring tag;                       // the tag being read (HTML: script and style hold code)
    auto until = [&](const wchar_t* close, Tok t) {
        size_t e = find(s, n, i, close);
        if (e == npos) { out(i, n, t); i = n; return false; }
        e += wcslen(close);
        out(i, e, t);
        i = e;
        return true;
    };
    auto code = [&](bool script) {          // up to </script> or </style>, as JavaScript or CSS
        size_t e = i;
        const wchar_t* close = script ? L"</script" : L"</style";
        while (e < n && !(s[e] == L'<' && at_nocase(s, n, e, close))) ++e;
        Out o = out;
        o.base += i;
        if (script) lex_generic(compiled(Lang::JavaScript), s + i, e - i, 0, o);
        else lex_css(s + i, e - i, 0, o);
        i = e;
        return e < n;
    };
    switch (state) {
        case 2: if (!until(L"-->", Tok::Comment)) return 2; state = 0; break;
        case 3: if (!until(L"]]>", Tok::String)) return 3; state = 0; break;
        case 6: if (!until(L"?>", Tok::Preproc)) return 6; state = 0; break;
        case 7: if (!until(L">", Tok::Preproc)) return 7; state = 0; break;
        case 4: case 5: if (!until(state == 4 ? L"\"" : L"'", Tok::String)) return state; state = 1; break;
        case 8: case 9: if (!code(state == 8)) return state; state = 0; break;
    }
    while (i < n) {
        wchar_t c = s[i];
        if (state == 1) {
            if (c == L'>' || (c == L'/' && i + 1 < n && s[i + 1] == L'>')) {
                size_t len = c == L'>' ? 1 : 2;
                out(i, i + len, Tok::Tag);
                i += len;
                state = 0;
                if (html && len == 1 && (tag == L"script" || tag == L"style")) {
                    bool script = tag == L"script";
                    if (!code(script)) return script ? 8 : 9;
                }
                continue;
            }
            if (c == L'"' || c == L'\'') {
                size_t e = i + 1;
                while (e < n && s[e] != c) ++e;
                if (e >= n) { out(i, n, Tok::String); return c == L'"' ? 4 : 5; }
                out(i, e + 1, Tok::String);
                i = e + 1;
                continue;
            }
            if (name_char(c)) {
                size_t j = i;
                while (j < n && name_char(s[j])) ++j;
                out(i, j, Tok::Attr);
                i = j;
                continue;
            }
            ++i;
            continue;
        }
        if (c == L'<') {
            if (at(s, n, i, L"<!--")) { if (!until(L"-->", Tok::Comment)) return 2; continue; }
            if (at(s, n, i, L"<![CDATA[")) { if (!until(L"]]>", Tok::String)) return 3; continue; }
            if (at(s, n, i, L"<?")) { if (!until(L"?>", Tok::Preproc)) return 6; continue; }
            if (at(s, n, i, L"<!")) { if (!until(L">", Tok::Preproc)) return 7; continue; }
            size_t j = i + 1;
            if (j < n && s[j] == L'/') ++j;
            if (j < n && alpha(s[j])) {
                size_t b = j;
                while (j < n && name_char(s[j])) ++j;
                tag.assign(s + b, j - b);
                for (auto& ch : tag) ch = (wchar_t)towlower(ch);
                if (s[i + 1] == L'/') tag.clear();
                out(i, j, Tok::Tag);
                i = j;
                state = 1;
                continue;
            }
        }
        if (c == L'&') {
            size_t j = i + 1;
            while (j < n && j - i < 12 && (alnum(s[j]) || s[j] == L'#')) ++j;
            if (j < n && s[j] == L';' && j > i + 1) { out(i, j + 1, Tok::Literal); i = j + 1; continue; }
        }
        ++i;
    }
    return state;
}

// ------------------------------------------------------------------
// CSS (and SCSS, Less)
// ------------------------------------------------------------------
// states: the depth of braces | 0x100 in a comment
int lex_css(const wchar_t* s, size_t n, int state, Out out) {
    int depth = state & 0xFF;
    size_t i = 0;
    if (state & 0x100) {
        size_t e = find(s, n, 0, L"*/");
        if (e == npos) { out(0, n, Tok::Comment); return state; }
        out(0, e + 2, Tok::Comment);
        i = e + 2;
    }
    auto ident = [&](size_t j) {
        while (j < n && (alnum(s[j]) || s[j] == L'-')) ++j;
        return j;
    };
    while (i < n) {
        wchar_t c = s[i];
        if (c == L'/' && i + 1 < n && s[i + 1] == L'*') {
            size_t e = find(s, n, i + 2, L"*/");
            if (e == npos) { out(i, n, Tok::Comment); return 0x100 | depth; }
            out(i, e + 2, Tok::Comment);
            i = e + 2;
            continue;
        }
        if (c == L'/' && i + 1 < n && s[i + 1] == L'/' && (i == 0 || space(s[i - 1]) || eol(s[i - 1]))) {
            size_t e = line_end(s, n, i);
            out(i, e, Tok::Comment);
            i = e;
            continue;
        }
        if (c == L'"' || c == L'\'') {
            size_t j = i + 1;
            while (j < n && s[j] != c && !eol(s[j])) j += s[j] == L'\\' ? 2 : 1;
            j = j < n && s[j] == c ? j + 1 : std::min(j, n);
            out(i, j, Tok::String);
            i = j;
            continue;
        }
        if (c == L'{') { if (depth < 255) ++depth; ++i; continue; }
        if (c == L'}') { if (depth > 0) --depth; ++i; continue; }
        if (c == L'@' && i + 1 < n && alpha(s[i + 1])) {
            size_t j = ident(i + 1);
            out(i, j, Tok::Keyword);
            i = j;
            continue;
        }
        if ((c == L'$' && i + 1 < n && alpha(s[i + 1])) || (c == L'-' && i + 2 < n && s[i + 1] == L'-' && alpha(s[i + 2]))) {
            size_t j = ident(i + 1);
            out(i, j, Tok::Variable);
            i = j;
            continue;
        }
        if (digit(c) || ((c == L'.' || c == L'-') && i + 1 < n && digit(s[i + 1]) && (i == 0 || !alnum(s[i - 1])))) {
            size_t j = i + 1;
            while (j < n && (alnum(s[j]) || s[j] == L'.' || s[j] == L'%')) ++j;
            out(i, j, Tok::Number);
            i = j;
            continue;
        }
        if (c == L'!' && at_nocase(s, n, i + 1, L"important")) {
            out(i, i + 10, Tok::Keyword);
            i += 10;
            continue;
        }
        // a selector's part, or a property or a value's word
        bool inRule = depth > 0;
        size_t k = i;
        if (inRule) {
            if (c == L'#' && i + 1 < n && hexdigit(s[i + 1])) {
                size_t j = i + 1;
                while (j < n && alnum(s[j])) ++j;
                out(i, j, Tok::Number);
                i = j;
                continue;
            }
            if (alpha(c) || c == L'-') {
                size_t j = ident(i);
                size_t after = skip_space(s, n, j);
                // a property (followed by ':' and not a nested selector: no '{' before ';' on its line)
                bool prop = false;
                if (after < n && s[after] == L':') {
                    prop = true;
                    for (size_t q = after; q < n && !eol(s[q]) && s[q] != L';' && s[q] != L'}'; ++q)
                        if (s[q] == L'{') { prop = false; break; }
                }
                if (prop) out(i, j, Tok::Key);
                else if (after < n && s[after] == L'(') out(i, j, Tok::Function);
                else if (!prop && after < n && (s[after] == L'{' || s[after] == L',')) out(i, j, Tok::Tag);
                i = j;
                continue;
            }
        }
        if (c == L'.' || c == L'#') {
            k = i + 1;
            if (k < n && (alpha(s[k]) || s[k] == L'-')) {
                size_t j = ident(k);
                out(i, j, Tok::Attr);
                i = j;
                continue;
            }
        }
        if (c == L':' && !inRule) {
            size_t j = i + 1;
            if (j < n && s[j] == L':') ++j;
            size_t e = ident(j);
            out(i, e, Tok::Keyword);
            i = std::max(e, i + 1);
            continue;
        }
        if (!inRule && (alpha(c) || c == L'*')) {
            size_t j = c == L'*' ? i + 1 : ident(i);
            out(i, j, Tok::Tag);
            i = j;
            continue;
        }
        ++i;
    }
    return depth;
}

// ------------------------------------------------------------------
// Batch, Diff
// ------------------------------------------------------------------
const Words& batch_words() {
    static const Words w(L"echo set if else goto call for in do not exist defined errorlevel equ neq lss leq gtr geq "
                         L"setlocal endlocal enabledelayedexpansion enableextensions shift exit pushd popd cd chdir md "
                         L"mkdir rd rmdir del erase copy xcopy move ren rename type start title cls pause choice "
                         L"findstr find where timeout color prompt path ver vol assoc ftype nul con",
                         true);
    return w;
}

int lex_batch(const wchar_t* s, size_t n, Out out) {
    size_t i = 0;
    while (i < n) {
        size_t e = line_end(s, n, i);
        size_t k = skip_space(s, e, i);
        while (k < e && s[k] == L'@') ++k;
        if (k < e && ((at_nocase(s, e, k, L"rem") && (k + 3 == e || space(s[k + 3]))) || at(s, e, k, L"::"))) {
            out(k, e, Tok::Comment);
        } else if (k < e && s[k] == L':') {
            out(k, e, Tok::Tag);
        } else {
            size_t j = k;
            while (j < e) {
                wchar_t c = s[j];
                if (c == L'"') {
                    size_t q = j + 1;
                    while (q < e && s[q] != L'"') ++q;
                    q = q < e ? q + 1 : e;
                    // (a variable inside still shows as one)
                    out(j, q, Tok::String);
                    j = q;
                    continue;
                }
                if (c == L'%') {
                    size_t q = j + 1;
                    if (q < e && s[q] == L'%') {                    // %%i
                        out(j, std::min(q + 2, e), Tok::Variable);
                        j = q + 2;
                        continue;
                    }
                    if (q < e && (digit(s[q]) || s[q] == L'*')) { out(j, q + 1, Tok::Variable); j = q + 1; continue; }
                    if (q < e && s[q] == L'~') {                    // %~dp0
                        size_t r = q + 1;
                        while (r < e && alpha(s[r])) ++r;
                        if (r < e && digit(s[r])) { out(j, r + 1, Tok::Variable); j = r + 1; continue; }
                    }
                    size_t r = q;
                    while (r < e && s[r] != L'%') ++r;
                    if (r < e && r > q) { out(j, r + 1, Tok::Variable); j = r + 1; continue; }
                    ++j;
                    continue;
                }
                if (c == L'!') {
                    size_t r = j + 1;
                    while (r < e && s[r] != L'!' && !space(s[r])) ++r;
                    if (r < e && s[r] == L'!' && r > j + 1) { out(j, r + 1, Tok::Variable); j = r + 1; continue; }
                    ++j;
                    continue;
                }
                if (alpha(c)) {
                    size_t r = j;
                    while (r < e && (alnum(s[r]) || s[r] == L'-' || s[r] == L'.')) ++r;
                    if (batch_words().has(std::wstring_view(s + j, r - j))) out(j, r, Tok::Keyword);
                    j = r;
                    continue;
                }
                if (digit(c) && (j == k || space(s[j - 1]))) {
                    size_t r = j;
                    while (r < e && alnum(s[r])) ++r;
                    if (r == e || space(s[r])) out(j, r, Tok::Number);
                    j = r;
                    continue;
                }
                ++j;
            }
        }
        i = e < n ? e + 1 : e;
    }
    return 0;
}

int lex_diff(const wchar_t* s, size_t n, Out out) {
    size_t i = 0;
    while (i < n) {
        size_t e = line_end(s, n, i);
        if (e > i) {
            if (at(s, e, i, L"+++ ") || at(s, e, i, L"--- ") || at(s, e, i, L"diff ") || at(s, e, i, L"index ")) out(i, e, Tok::Heading);
            else if (s[i] == L'+') out(i, e, Tok::Added);
            else if (s[i] == L'-') out(i, e, Tok::Removed);
            else if (at(s, e, i, L"@@")) out(i, e, Tok::Preproc);
            else if (s[i] == L'\\') out(i, e, Tok::Comment);
        }
        i = e < n ? e + 1 : e;
    }
    return 0;
}

// ------------------------------------------------------------------
// the languages
// ------------------------------------------------------------------
struct LangInfo {
    Lang lang;
    const wchar_t* key;
    const wchar_t* name;
    const wchar_t* exts;                    // extensions (and whole names of files without one)
    const Spec* spec;                       // (the C-like lexer's set-up; null: a lexer of its own)
};

const LangInfo kLangs[] = {
    { Lang::None, L"none", L"Plain text", L"", nullptr },
    { Lang::C, L"c", L"C / C++", L"c h cpp hpp cc cxx hh hxx inl ipp ino m mm", &kC },
    { Lang::CSharp, L"csharp", L"C#", L"cs csx", &kCSharp },
    { Lang::Java, L"java", L"Java / Kotlin", L"java kt kts gradle groovy scala", &kJava },
    { Lang::JavaScript, L"javascript", L"JavaScript", L"js mjs cjs jsx", &kJavaScript },
    { Lang::TypeScript, L"typescript", L"TypeScript", L"ts tsx mts cts", &kTypeScript },
    { Lang::Go, L"go", L"Go", L"go", &kGo },
    { Lang::Rust, L"rust", L"Rust", L"rs", &kRust },
    { Lang::Python, L"python", L"Python", L"py pyw pyi", &kPython },
    { Lang::Ruby, L"ruby", L"Ruby", L"rb rake gemspec", &kRuby },
    { Lang::Lua, L"lua", L"Lua", L"lua", &kLua },
    { Lang::PowerShell, L"powershell", L"PowerShell", L"ps1 psm1 psd1", &kPowerShell },
    { Lang::Batch, L"batch", L"Batch", L"bat cmd", nullptr },
    { Lang::Shell, L"shell", L"Shell", L"sh bash zsh ksh bashrc zshrc profile dockerfile makefile", &kShell },
    { Lang::Sql, L"sql", L"SQL", L"sql", &kSql },
    { Lang::Css, L"css", L"CSS", L"css scss less", nullptr },
    { Lang::Json, L"json", L"JSON", L"json jsonc json5 geojson webmanifest har", &kJson },
    { Lang::Xml, L"xml", L"XML", L"xml xaml xsd xsl xslt svg csproj vcxproj fsproj vbproj props targets nuspec resx config "
                                 L"manifest plist kml gpx rss atom wsdl dtd", nullptr },
    { Lang::Html, L"html", L"HTML", L"html htm xhtml vue svelte", nullptr },
    { Lang::Markdown, L"markdown", L"Markdown", L"md markdown mdown mkd", nullptr },
    { Lang::Ini, L"ini", L"INI", L"ini inf cfg conf properties reg editorconfig gitconfig gitmodules desktop url env npmrc", nullptr },
    { Lang::Toml, L"toml", L"TOML", L"toml", nullptr },
    { Lang::Yaml, L"yaml", L"YAML", L"yml yaml", nullptr },
    { Lang::Diff, L"diff", L"Diff", L"diff patch", nullptr },
};
static_assert(sizeof(kLangs) / sizeof(kLangs[0]) == (size_t)Lang::Count);

const Compiled& compiled(Lang l) {
    static std::unique_ptr<Compiled> cache[(size_t)Lang::Count];
    auto& c = cache[(size_t)l];
    if (!c) c = std::make_unique<Compiled>(kLangs[(size_t)l].spec ? kLangs[(size_t)l].spec : &kC);
    return *c;
}

} // namespace


const wchar_t* lang_name(Lang l) { return l < Lang::Count ? kLangs[(size_t)l].name : L""; }
const wchar_t* lang_key(Lang l) { return l < Lang::Count ? kLangs[(size_t)l].key : L"none"; }

Lang lang_from_key(const std::wstring& key) {
    for (const auto& li : kLangs)
        if (key == li.key) return li.lang;
    return Lang::None;
}

Lang lang_for_file(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    std::wstring name = path.substr(slash == std::wstring::npos ? 0 : slash + 1);
    for (auto& c : name) c = (wchar_t)towlower(c);
    size_t dot = name.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos ? name : name.substr(dot + 1);
    if (ext.empty()) return Lang::None;
    for (const auto& li : kLangs) {
        for (const wchar_t* p = li.exts; *p;) {
            while (*p == L' ') ++p;
            const wchar_t* q = p;
            while (*p && *p != L' ') ++p;
            if ((size_t)(p - q) == ext.size() && wcsncmp(q, ext.c_str(), ext.size()) == 0) return li.lang;
        }
    }
    return Lang::None;
}

int syntax_lex(Lang l, const wchar_t* s, size_t n, int state, std::vector<SyntaxSpan>* out) {
    Out o{ out };
    switch (l) {
        case Lang::None: return 0;
        case Lang::Batch: return lex_batch(s, n, o);
        case Lang::Css: return lex_css(s, n, state, o);
        case Lang::Xml: return lex_xml(s, n, state, false, o);
        case Lang::Html: return lex_xml(s, n, state, true, o);
        case Lang::Markdown: return lex_markdown(s, n, state, o);
        case Lang::Ini: return lex_ini(s, n, state, false, o);
        case Lang::Toml: return lex_ini(s, n, state, true, o);
        case Lang::Yaml: return lex_yaml(s, n, state, o);
        case Lang::Diff: return lex_diff(s, n, o);
        default: return l < Lang::Count ? lex_generic(compiled(l), s, n, state, o) : 0;
    }
}

// ------------------------------------------------------------------
// formatting
// ------------------------------------------------------------------
namespace {

bool ws(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == 0xFEFF; }

// a new line at `depth`, unless the output is at one already
void new_line(std::wstring& out, size_t depth) {
    while (!out.empty() && (out.back() == L' ' || out.back() == L'\t')) out.pop_back();
    if (!out.empty() && out.back() != L'\n') out += L"\r\n";
    out.append(depth * 4, L' ');
}

} // namespace

bool format_json(const std::wstring& in, bool compact, std::wstring& out, size_t* errorAt) {
    out.clear();
    out.reserve(in.size() + in.size() / 2);
    std::vector<wchar_t> stack;
    size_t i = 0, n = in.size(), lastEnd = 0;
    bool afterValue = false;                // a value just ended: a comma, a colon or a bracket comes next
    auto fail = [&](size_t at) {
        if (errorAt) *errorAt = at;
        return false;
    };
    while (i < n) {
        wchar_t c = in[i];
        if (ws(c)) { ++i; continue; }
        if (c == L'/' && i + 1 < n && (in[i + 1] == L'/' || in[i + 1] == L'*')) {
            bool line = in[i + 1] == L'/';
            size_t e = i + 2;
            if (line) {
                while (e < n && !eol(in[e])) ++e;
            } else {
                e = in.find(L"*/", i + 2);
                if (e == std::wstring::npos) return fail(i);
                e += 2;
            }
            if (!compact) {
                // on the line of what came before: kept there
                bool sameLine = lastEnd > 0 && in.find_first_of(L"\r\n", lastEnd) >= i;
                if (sameLine) {
                    while (!out.empty() && (out.back() == L' ' || out.back() == L'\n' || out.back() == L'\r')) out.pop_back();
                    out += L' ';
                }
                out.append(in, i, e - i);
                if (line || sameLine) new_line(out, stack.size());
            }
            lastEnd = e;
            i = e;
            continue;
        }
        if (c == L'"' || c == L'\'') {
            if (afterValue) return fail(i);
            size_t j = i + 1;
            while (j < n && in[j] != c) {
                if (in[j] == L'\\') ++j;
                else if (eol(in[j])) return fail(i);
                ++j;
            }
            if (j >= n) return fail(i);
            out.append(in, i, j + 1 - i);
            i = lastEnd = j + 1;
            afterValue = true;
            continue;
        }
        if (c == L'{' || c == L'[') {
            if (afterValue || stack.size() >= 1000) return fail(i);   // (each level indents every line below it)
            stack.push_back(c == L'{' ? L'}' : L']');
            out += c;
            size_t k = i + 1;
            while (k < n && ws(in[k])) ++k;
            if (k < n && in[k] == stack.back()) {          // ({} and [] stay so)
                out += in[k];
                stack.pop_back();
                i = lastEnd = k + 1;
                afterValue = true;
                continue;
            }
            if (!compact) new_line(out, stack.size());
            i = lastEnd = i + 1;
            afterValue = false;
            continue;
        }
        if (c == L'}' || c == L']') {
            if (stack.empty() || stack.back() != c) return fail(i);
            stack.pop_back();
            if (!compact) new_line(out, stack.size());
            out += c;
            i = lastEnd = i + 1;
            afterValue = true;
            continue;
        }
        if (c == L',') {
            if (stack.empty() || !afterValue) return fail(i);
            afterValue = false;
            out += L',';
            if (!compact) new_line(out, stack.size());
            i = lastEnd = i + 1;
            continue;
        }
        if (c == L':') {
            if (stack.empty() || stack.back() != L'}' || !afterValue) return fail(i);
            afterValue = false;
            out += compact ? L":" : L": ";
            i = lastEnd = i + 1;
            continue;
        }
        // a number, true, false, null (JSON5: a bare name)
        size_t j = i;
        while (j < n && !ws(in[j]) && !has_char(L"{}[],:\"'/", in[j])) ++j;
        if (j == i || afterValue) return fail(i);
        out.append(in, i, j - i);
        i = lastEnd = j;
        afterValue = true;
    }
    if (!stack.empty()) return fail(n);
    while (!out.empty() && ws(out.back())) out.pop_back();
    return true;
}

bool format_xml(const std::wstring& in, std::wstring& out, size_t* errorAt) {
    enum Kind { Text, Open, Close, Empty, Other };
    struct Node { Kind kind; size_t a, b; std::wstring name; };
    std::vector<Node> nodes;
    size_t i = 0, n = in.size();
    auto fail = [&](size_t at) {
        if (errorAt) *errorAt = at;
        return false;
    };
    auto tag_name = [&](size_t j) {
        size_t b = j;
        while (j < n && name_char(in[j])) ++j;
        return in.substr(b, j - b);
    };
    while (i < n) {
        if (in[i] != L'<') {
            size_t j = in.find(L'<', i);
            if (j == std::wstring::npos) j = n;
            nodes.push_back({ Text, i, j, {} });
            i = j;
            continue;
        }
        auto other = [&](const wchar_t* close) {
            size_t e = in.find(close, i);
            if (e == std::wstring::npos) return false;
            nodes.push_back({ Other, i, e + wcslen(close), {} });
            i = e + wcslen(close);
            return true;
        };
        if (in.compare(i, 4, L"<!--") == 0) { if (!other(L"-->")) return fail(i); continue; }
        if (in.compare(i, 9, L"<![CDATA[") == 0) { if (!other(L"]]>")) return fail(i); continue; }
        if (in.compare(i, 2, L"<?") == 0) { if (!other(L"?>")) return fail(i); continue; }
        if (in.compare(i, 2, L"<!") == 0) {                 // <!DOCTYPE ...[ ... ]>
            size_t j = i + 2;
            int depth = 0;
            while (j < n && !(in[j] == L'>' && depth == 0)) {
                if (in[j] == L'[') ++depth;
                else if (in[j] == L']') --depth;
                ++j;
            }
            if (j >= n) return fail(i);
            nodes.push_back({ Other, i, j + 1, {} });
            i = j + 1;
            continue;
        }
        bool close = i + 1 < n && in[i + 1] == L'/';
        std::wstring name = tag_name(i + (close ? 2 : 1));
        if (name.empty()) return fail(i);
        size_t j = i + 1;
        wchar_t quote = 0;
        for (; j < n; ++j) {
            if (quote) { if (in[j] == quote) quote = 0; }
            else if (in[j] == L'"' || in[j] == L'\'') quote = in[j];
            else if (in[j] == L'>') break;
            else if (in[j] == L'<') return fail(i);
        }
        if (j >= n) return fail(i);
        nodes.push_back({ close ? Close : in[j - 1] == L'/' ? Empty : Open, i, j + 1, name });
        i = j + 1;
    }
    out.clear();
    out.reserve(in.size() + in.size() / 4);
    std::vector<size_t> open;                           // nodes[] of the elements open
    auto trimmed = [&](const Node& nd) {
        size_t a = nd.a, b = nd.b;
        while (a < b && ws(in[a])) ++a;
        while (b > a && ws(in[b - 1])) --b;
        return std::make_pair(a, b);
    };
    auto line = [&](size_t a, size_t b) {
        if (!out.empty()) out += L"\r\n";
        out.append(open.size() * 4, L' ');
        out.append(in, a, b - a);
    };
    for (size_t k = 0; k < nodes.size(); ++k) {
        const Node& nd = nodes[k];
        switch (nd.kind) {
            case Text: {
                auto [a, b] = trimmed(nd);
                if (b > a) line(a, b);
                break;
            }
            case Open: {
                // <a>text</a> and <a></a>: on one line
                if (k + 2 < nodes.size() && nodes[k + 1].kind == Text && nodes[k + 2].kind == Close && nodes[k + 2].name == nd.name) {
                    auto [a, b] = trimmed(nodes[k + 1]);
                    line(nd.a, nd.b);
                    out.append(in, a, b - a);
                    out.append(in, nodes[k + 2].a, nodes[k + 2].b - nodes[k + 2].a);
                    k += 2;
                    break;
                }
                if (k + 1 < nodes.size() && nodes[k + 1].kind == Close && nodes[k + 1].name == nd.name) {
                    line(nd.a, nd.b);
                    out.append(in, nodes[k + 1].a, nodes[k + 1].b - nodes[k + 1].a);
                    k += 1;
                    break;
                }
                line(nd.a, nd.b);
                open.push_back(k);
                break;
            }
            case Close:
                if (open.empty() || nodes[open.back()].name != nd.name) return fail(nd.a);
                open.pop_back();
                line(nd.a, nd.b);
                break;
            default:
                line(nd.a, nd.b);
                break;
        }
    }
    if (!open.empty()) return fail(nodes[open.back()].a);
    return true;
}
