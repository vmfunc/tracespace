// Theme-dependent glyphs shared by the layout (label widths) and the painter.
#pragma once

#include <string>

namespace tracespace {

enum class Theme { Braille, Rounded, Ascii };
enum class OrderMode { Execution, Source };

const char* theme_name(Theme t);
bool parse_theme(const std::string& s, Theme* out);

struct Glyphs {
    char32_t h, v, tl, tr, bl, br;  // box border
    char32_t dh, dv;                // dashed border / dashed line
    char32_t right, up, down;       // arrowheads
    char32_t ret, brk, cont;        // early-exit markers
    char32_t rec, warn, collapsed;  // box label markers
    char32_t times, ellipsis, runner, dot;
    char32_t goto_site, label_site;
    const char* inf;
    char32_t junction[16];  // indexed by N=1 | E=2 | S=4 | W=8
};

const Glyphs& glyphs(Theme t);

// UTF-8 helpers. Every code point is assumed to be one column wide.
std::string utf8(char32_t c);
int ulen(const std::string& s);
std::u32string decode(const std::string& s);
std::string truncate(const std::string& s, int max_cols, char32_t ellipsis);

}  // namespace tracespace
