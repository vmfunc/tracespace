#include "core/glyphs.h"

namespace tracespace {

const char* theme_name(Theme t) {
    switch (t) {
        case Theme::Braille: return "braille";
        case Theme::Rounded: return "rounded";
        case Theme::Ascii: return "ascii";
    }
    return "?";
}

bool parse_theme(const std::string& s, Theme* out) {
    if (s == "braille") *out = Theme::Braille;
    else if (s == "rounded") *out = Theme::Rounded;
    else if (s == "ascii") *out = Theme::Ascii;
    else return false;
    return true;
}

const Glyphs& glyphs(Theme t) {
    static const Glyphs unicode = {
        U'─', U'│', U'┌', U'┐', U'└', U'┘',
        U'╌', U'╎',
        U'▶', U'▲', U'▼',
        U'⏎', U'⇥', U'↺',
        U'↻', U'⚠', U'▸',
        U'×', U'…', U'●', U'·',
        U'○', U'◆',
        "∞",
        {U' ', U'│', U'─', U'└', U'│', U'│', U'┌', U'├',
         U'─', U'┘', U'─', U'┴', U'┐', U'┤', U'┬', U'┼'},
    };
    static const Glyphs ascii = {
        U'-', U'|', U'+', U'+', U'+', U'+',
        U'.', U':',
        U'>', U'^', U'v',
        U'^', U'!', U'@',
        U'*', U'!', U'>',
        U'x', U'~', U'o', U'.',
        U'o', U'#',
        "inf",
        {U' ', U'|', U'-', U'+', U'|', U'|', U'+', U'+',
         U'-', U'+', U'-', U'+', U'+', U'+', U'+', U'+'},
    };
    return t == Theme::Ascii ? ascii : unicode;
}

std::string utf8(char32_t c) {
    std::string s;
    if (c < 0x80) {
        s += (char)c;
    } else if (c < 0x800) {
        s += (char)(0xC0 | (c >> 6));
        s += (char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        s += (char)(0xE0 | (c >> 12));
        s += (char)(0x80 | ((c >> 6) & 0x3F));
        s += (char)(0x80 | (c & 0x3F));
    } else {
        s += (char)(0xF0 | (c >> 18));
        s += (char)(0x80 | ((c >> 12) & 0x3F));
        s += (char)(0x80 | ((c >> 6) & 0x3F));
        s += (char)(0x80 | (c & 0x3F));
    }
    return s;
}

std::u32string decode(const std::string& s) {
    std::u32string out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        int n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
        char32_t cp = n == 1 ? c : c & (0xFF >> (n + 1));
        for (int k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        out += cp;
        i += (size_t)n;
    }
    return out;
}

int ulen(const std::string& s) { return (int)decode(s).size(); }

std::string truncate(const std::string& s, int max_cols, char32_t ellipsis) {
    std::u32string u = decode(s);
    if ((int)u.size() <= max_cols) return s;
    std::string out;
    for (int i = 0; i < max_cols - 1; ++i) out += utf8(u[(size_t)i]);
    return out + utf8(ellipsis);
}

}  // namespace tracespace
