#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include "core/layout.h"
#include "core/model.h"
#include "core/paint.h"
#include "core/parser.h"
#include "io/watcher.h"
#ifdef TRACESPACE_TUI
#include "tui/app.h"
#endif

namespace {

using namespace tracespace;

struct Options {
    std::string file, socket_path, jump_cmd, frame;
    bool dump = false, sexp = false, print = false, bench = false, help = false;
    bool show_library = false, animate = true, color = true;
    Theme theme = Theme::Braille;
    OrderMode order = OrderMode::Execution;
};

const char* kUsage =
    "usage: tracespace [options] <file>\n"
    "       tracespace --socket <path> [options] [file]\n"
    "\n"
    "Draws a live diagram of how a C/C++ file executes. With no mode option it\n"
    "opens the interactive view and redraws whenever the file is saved.\n"
    "\n"
    "  --socket PATH    listen for an editor (tracespace.nvim) on a Unix socket\n"
    "                   (a named pipe on Windows); its buffer overrides the file\n"
    "  --theme NAME     arc theme: braille (default), rounded, ascii\n"
    "  --order MODE     execution (default) or source\n"
    "  --lib            show calls to functions not defined in the file\n"
    "  --jump-cmd CMD   run CMD on Enter when no editor is connected;\n"
    "                   {file}, {line} and {col} are substituted\n"
    "  --no-anim        do not animate the selected loop\n"
    "  --no-color       monochrome output (also when NO_COLOR is set)\n"
    "  --print          print the diagram as plain text and exit\n"
    "  --dump           print the parsed model (IR) as indented text and exit\n"
    "  --help           show this help\n"
    "\n"
    "Press ? inside the interactive view for keys.\n";

// True when the terminal can show UTF-8; otherwise the ascii theme is forced.
bool locale_is_utf8() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);  // fails harmlessly when output is a file or pipe
    return true;
#else
    for (const char* name : {"LC_ALL", "LC_CTYPE", "LANG"}) {
        const char* v = std::getenv(name);
        if (!v || !*v) continue;
        std::string s = v;
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        return s.find("utf-8") != std::string::npos || s.find("utf8") != std::string::npos;
    }
    return false;
#endif
}

bool parse_args(int argc, char** argv, Options* o, std::string* err) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto value = [&](std::string* out) {
            if (i + 1 >= argc) {
                *err = a + " needs a value";
                return false;
            }
            *out = argv[++i];
            return true;
        };
        std::string v;
        if (a == "--dump") o->dump = true;
        else if (a == "--sexp") o->sexp = true;
        else if (a == "--print") o->print = true;
        else if (a == "--bench") o->bench = true;
        else if (a == "--lib") o->show_library = true;
        else if (a == "--no-anim") o->animate = false;
        else if (a == "--no-color") o->color = false;
        else if (a == "--help" || a == "-h") o->help = true;
        else if (a == "--socket") {
            if (!value(&o->socket_path)) return false;
        } else if (a == "--jump-cmd") {
            if (!value(&o->jump_cmd)) return false;
        } else if (a == "--frame") {
            if (!value(&o->frame)) return false;
        } else if (a == "--theme") {
            if (!value(&v)) return false;
            if (!parse_theme(v, &o->theme)) {
                *err = "unknown theme: " + v + " (expected braille, rounded or ascii)";
                return false;
            }
        } else if (a == "--order") {
            if (!value(&v)) return false;
            if (v == "execution") o->order = OrderMode::Execution;
            else if (v == "source") o->order = OrderMode::Source;
            else {
                *err = "unknown order: " + v + " (expected execution or source)";
                return false;
            }
        } else if (!a.empty() && a[0] == '-') {
            *err = "unknown option: " + a;
            return false;
        } else if (o->file.empty()) {
            o->file = a;
        } else {
            *err = "more than one file given";
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    std::string err;
    if (!parse_args(argc, argv, &opt, &err)) {
        std::fprintf(stderr, "tracespace: %s\n\n%s", err.c_str(), kUsage);
        return 2;
    }
    if (opt.help) {
        std::fputs(kUsage, stdout);
        return 0;
    }
    bool utf8 = locale_is_utf8();
    if (!utf8) opt.theme = Theme::Ascii;
    if (const char* nc = std::getenv("NO_COLOR"); nc && *nc) opt.color = false;

    bool batch = opt.dump || opt.sexp || opt.print || opt.bench;
    if (opt.file.empty() && (batch || opt.socket_path.empty())) {
        std::fprintf(stderr, "tracespace: no input file\n\n%s", kUsage);
        return 2;
    }

    if (batch) {
        std::string text;
        if (!read_file(opt.file, &text)) {
            std::fprintf(stderr, "tracespace: cannot read %s\n", opt.file.c_str());
            return 1;
        }
        if (opt.sexp) {  // undocumented: the raw tree-sitter tree
            std::cout << debug_sexp(opt.file, text) << "\n";
            return 0;
        }
        auto parser = make_tree_sitter_parser();
        Model model = parser->parse(opt.file, text);
        if (opt.dump) {
            std::cout << dump(model);
            return 0;
        }
        ViewState view;
        view.theme = opt.theme;
        view.order = opt.order;
        view.show_library = opt.show_library;
        if (opt.bench) {  // undocumented: time a full parse and layout of the file
            using clock = std::chrono::steady_clock;
            auto ms = [](clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
            const int runs = 20;
            clock::duration parse_time{}, layout_time{};
            for (int i = 0; i < runs; ++i) {
                auto t0 = clock::now();
                Model m = parser->parse(opt.file, text);
                auto t1 = clock::now();
                Layout l = compute_layout(m, view);
                auto t2 = clock::now();
                parse_time += t1 - t0;
                layout_time += t2 - t1;
            }
            std::printf("parse %.2f ms, layout %.2f ms (mean of %d runs)\n", ms(parse_time) / runs,
                        ms(layout_time) / runs, runs);
            return 0;
        }
        PaintOptions po;
        po.theme = view.theme;
        std::cout << paint(compute_layout(model, view), po).to_text();
        return 0;
    }

#ifdef TRACESPACE_TUI
    AppOptions app;
    app.file = opt.file;
    app.socket_path = opt.socket_path;
    app.jump_cmd = opt.jump_cmd;
    app.theme = opt.theme;
    app.order = opt.order;
    app.show_library = opt.show_library;
    app.animate = opt.animate;
    app.utf8 = utf8;
    app.color = opt.color;
    if (!opt.frame.empty()) {  // undocumented: one frame as text, "WxH" or "WxH:help"
        int w = 0, h = 0;
        if (std::sscanf(opt.frame.c_str(), "%dx%d", &w, &h) != 2 || w < 1 || h < 2) {
            std::fprintf(stderr, "tracespace: --frame expects WxH\n");
            return 2;
        }
        bool help = opt.frame.find(":help") != std::string::npos;
        std::cout << render_frame(app, w, h, help);
        return 0;
    }
    return run_app(app);
#else
    std::fprintf(stderr, "tracespace: built without the interactive view; use --print or --dump\n");
    return 1;
#endif
}
