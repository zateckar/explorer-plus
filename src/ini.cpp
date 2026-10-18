#include "ini.h"
#include <cwchar>
#include <cwctype>

bool ini_ieq(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

std::wstring ini_trim(const std::wstring& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t')) ++b;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t')) --e;
    return s.substr(b, e - b);
}

std::wstring ini_decode(const std::string& raw) {
    auto widen = [&](UINT cp, size_t skip, DWORD flags, std::wstring& out) {
        int len = (int)(raw.size() - skip);
        if (len <= 0) { out.clear(); return true; }
        int n = MultiByteToWideChar(cp, flags, raw.data() + skip, len, nullptr, 0);
        if (n <= 0) return false;
        out.resize(n);
        MultiByteToWideChar(cp, flags, raw.data() + skip, len, out.data(), n);
        return true;
    };
    std::wstring text;
    // UTF-16 without a BOM (some editors and scripts write it): zero bytes at odd places only
    // (LE) or at even places only (BE), in the first kilobyte
    int oddZeros = 0, evenZeros = 0;
    for (size_t i = 0; i < raw.size() && i < 1024; ++i)
        if (!raw[i]) ++(i & 1 ? oddZeros : evenZeros);
    size_t sample = std::min<size_t>(raw.size(), 1024);
    bool le = raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE;
    bool be = raw.size() >= 2 && (unsigned char)raw[0] == 0xFE && (unsigned char)raw[1] == 0xFF;
    bool bareLe = !le && !be && sample >= 4 && oddZeros >= (int)sample / 4 && evenZeros == 0;
    bool bareBe = !le && !be && sample >= 4 && evenZeros >= (int)sample / 4 && oddZeros == 0;
    if (le || be || bareLe || bareBe) {
        size_t skip = le || be ? 2 : 0;
        text.resize((raw.size() - skip) / 2);
        for (size_t i = 0; i < text.size(); ++i) {
            unsigned char a = (unsigned char)raw[skip + 2 * i], b = (unsigned char)raw[skip + 2 * i + 1];
            text[i] = (wchar_t)(be || bareBe ? (a << 8) | b : (b << 8) | a);
        }
    } else if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB &&
               (unsigned char)raw[2] == 0xBF) {
        widen(CP_UTF8, 3, 0, text);
    } else if (!widen(CP_UTF8, 0, MB_ERR_INVALID_CHARS, text)) {
        widen(CP_ACP, 0, 0, text);
    }
    return text;
}

IniDoc ini_parse(const std::wstring& text) {
    IniDoc d;
    d.sections.push_back({ L"", {} });
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = nl == std::wstring::npos ? text.size() + 1 : nl + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        std::wstring t = ini_trim(line);
        if (t.size() >= 2 && t[0] == L'[' && t.find(L']') != std::wstring::npos) {
            d.sections.push_back({ ini_trim(t.substr(1, t.find(L']') - 1)), {} });
            continue;
        }
        IniLine l;
        size_t eq = t.find(L'=');
        if (!t.empty() && t[0] != L';' && eq != std::wstring::npos && eq > 0) {
            l.entry = true;
            l.key = ini_trim(t.substr(0, eq));
            l.value = ini_trim(t.substr(eq + 1));
            // (the profile APIs dropped quotes around a value: files they wrote read the same)
            if (l.value.size() >= 2 && l.value.front() == L'"' && l.value.back() == L'"')
                l.value = l.value.substr(1, l.value.size() - 2);
        } else {
            if (t.empty() && pos > text.size()) break;    // (the final newline)
            l.raw = line;
        }
        d.sections.back().lines.push_back(std::move(l));
    }
    return d;
}

std::wstring ini_serialize(const IniDoc& d) {
    std::wstring out;
    for (auto& s : d.sections) {
        if (!s.name.empty()) out += L"[" + s.name + L"]\r\n";
        for (auto& l : s.lines) out += (l.entry ? l.key + L"=" + l.value : l.raw) + L"\r\n";
    }
    return out;
}

std::wstring ini_escape_key(const std::wstring& k) {
    std::wstring out;
    for (size_t i = 0; i < k.size(); ++i) {
        WCHAR c = k[i];
        bool edgeBlank = (c == L' ' || c == L'\t') && (i == 0 || i + 1 == k.size());
        if (c == L'\\') out += L'|';
        else if (c == L'=' || c == L';' || c == L'[' || c == L']' || c == L'%' || c == L'|' ||
                 c == L'"' || edgeBlank || c < 0x20) {
            WCHAR hex[8];
            swprintf(hex, 8, L"%%%02X", (unsigned)c & 0xFF);
            out += hex;
        } else out += c;
    }
    return out;
}

std::wstring ini_unescape_key(const std::wstring& k) {
    std::wstring out;
    for (size_t i = 0; i < k.size(); ++i) {
        if (k[i] == L'|') out += L'\\';
        else if (k[i] == L'%' && i + 2 < k.size() && iswxdigit(k[i + 1]) && iswxdigit(k[i + 2])) {
            out += (WCHAR)wcstoul(k.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else out += k[i];
    }
    return out;
}
