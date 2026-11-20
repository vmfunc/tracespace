#include "tui/app.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/canvas.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include "core/parser.h"
#include "core/protocol.h"
#include "core/session.h"
#include "core/updater.h"
#include "io/transport.h"
#include "io/watcher.h"

namespace tracespace {

using namespace ftxui;

namespace {

Color palette(uint8_t c) {
    switch (c) {
        case 1: return Color::Cyan;
        case 2: return Color::Magenta;
        case 3: return Color::BlueLight;
        case 4: return Color::GreenLight;
        case 5: return Color::RedLight;
        case 6: return Color::White;
        case kColorSelect: return Color::Yellow;
        case kColorCursor: return Color::Green;
        case kColorError: return Color::Red;
        default: return Color::Default;
    }
}

// Copies the visible part of the grid onto an FTXUI canvas. Braille cells are
// drawn dot by dot with Canvas::DrawPoint (2x4 sub-cell resolution); everything
// else is text.
void draw_grid(Canvas& c, const Grid& g, int scroll_x, int scroll_y, bool color) {
    int view_w = c.width() / 2, view_h = c.height() / 4;
    for (int y = 0; y < view_h; ++y) {
        for (int x = 0; x < view_w; ++x) {
            const Cell* cell = g.at(x + scroll_x, y + scroll_y);
            if (!cell || cell->ch == U' ') continue;
            Cell v = *cell;
            auto style = [v, color](Pixel& p) {
                p.bold = v.attr & kBold;
                p.dim = v.attr & kDim;
                if (color) {
                    if (v.color != kColorDefault) p.foreground_color = palette(v.color);
                } else {
                    // monochrome fallback: selection inverted, cursor underlined
                    p.inverted = v.color == kColorSelect;
                    p.underlined = v.color == kColorCursor;
                }
            };
            if (v.braille) {
                static const uint8_t kBit[2][4] = {{0x01, 0x02, 0x04, 0x40}, {0x08, 0x10, 0x20, 0x80}};
                for (int dx = 0; dx < 2; ++dx)
                    for (int dy = 0; dy < 4; ++dy)
                        if (v.braille & kBit[dx][dy]) c.DrawPoint(x * 2 + dx, y * 4 + dy, true, style);
            } else {
                c.DrawText(x * 2, y * 4, utf8(v.ch), style);
            }
        }
    }
}

Element help_box(Theme theme) {
    const Glyphs& g = glyphs(theme);
    auto row = [](const std::string& keys, const std::string& what) {
        return hbox({text(keys) | bold | size(WIDTH, EQUAL, 16), text(what)});
    };
    auto mark = [&](char32_t glyph, const std::string& what) {
        return hbox({text(utf8(glyph)) | bold | size(WIDTH, EQUAL, 6), text(what)});
    };
    bool ascii = theme == Theme::Ascii;
    return vbox({
               text("tracespace") | bold | hcenter,
               separator(),
               row(ascii ? "arrows" : "← → ↑ ↓", "move the selection (click also selects)"),
               row("Enter", "jump to the selected box's code"),
               row("Space", "collapse / expand the selected box"),
               row("Tab", "execution order / source order"),
               row(ascii ? "Shift+arrows" : "Shift+← →", "scroll sideways (also [ ] and the mouse wheel)"),
               row("T", "cycle arc theme: braille, rounded, ascii"),
               row("L", "show / hide library calls"),
               row("?", "this help"),
               row("q", "quit"),
               separator(),
               hbox({text(utf8(g.times) + "N") | bold | size(WIDTH, EQUAL, 6),
                     text("loop count (" + utf8(g.times) + "? unknown, " + utf8(g.times) + g.inf + " endless)")}),
               mark(g.ret, "early return"),
               mark(g.brk, "break out of the loop"),
               mark(g.cont, "continue the loop"),
               mark(g.rec, "recursive call (not expanded again)"),
               mark(g.warn, "parse error inside this function"),
               mark(g.collapsed, "collapsed; the number counts hidden boxes"),
               hbox({text("?(x)") | bold | size(WIDTH, EQUAL, 6), text("call through a pointer or virtual method")}),
               hbox({text(utf8(g.goto_site) + utf8(g.dh) + utf8(g.label_site)) | bold | size(WIDTH, EQUAL, 6),
                     text("goto and its label")}),
               separator(),
               text("press any key to close") | dim | hcenter,
           }) |
           border;
}

struct Ui {
    Session session;
    bool color = true;
    bool show_help = false;
    std::string placeholder;  // shown while there is nothing to draw
    Box diagram_box;
    std::function<void(bool)> on_animation;  // told after each frame whether an arc is animating
    std::function<long()> tick;

    Element frame() {
        auto diagram = canvas([this](Canvas& c) {
            int view_w = c.width() / 2, view_h = c.height() / 4;
            session.fit(view_w, view_h);
            draw_grid(c, session.render(tick ? tick() : 0), session.scroll_x(), session.scroll_y(), color);
            if (on_animation) on_animation(session.animation_visible(view_w, view_h));
        });
        Element body = diagram | flex | reflect(diagram_box);
        if (session.layout().boxes.empty()) body = dbox({body, text(placeholder) | dim | center});
        Element page = vbox({body, hbox({text(session.status()), filler()}) | inverted});
        if (show_help) page = dbox({page, help_box(session.view.theme) | clear_under | center});
        return page;
    }
};

void configure(Ui& ui, const AppOptions& opt) {
    ui.session.utf8 = opt.utf8;
    ui.session.animate = opt.animate;
    ui.session.view.theme = opt.utf8 ? opt.theme : Theme::Ascii;
    ui.session.view.order = opt.order;
    ui.session.view.show_library = opt.show_library;
    ui.color = opt.color;
}

std::string connection_text(const AppOptions& opt, bool connected, Theme theme) {
    if (opt.socket_path.empty()) return "watching file";
    bool ascii = theme == Theme::Ascii;
    if (connected) return ascii ? "nvim *" : "nvim ●";
    return ascii ? "nvim - (waiting)" : "nvim ○ (waiting)";
}

void run_detached(const std::string& command) {
    // Keep the command's output off the screen. It is wrapped in parentheses so
    // that redirections inside the user's own command still take effect.
#ifdef _WIN32
    std::string quiet = "(" + command + ") >NUL 2>&1";
#else
    std::string quiet = "(" + command + ") >/dev/null 2>&1";
#endif
    std::thread([quiet] { (void)!std::system(quiet.c_str()); }).detach();
}

}  // namespace

std::string render_frame(const AppOptions& opt, int width, int height, bool help) {
    Ui ui;
    configure(ui, opt);
    ui.show_help = help;
    ui.session.connection = connection_text(opt, false, ui.session.view.theme);
    std::string text;
    if (!opt.file.empty() && read_file(opt.file, &text))
        ui.session.set_model(make_tree_sitter_parser()->parse(opt.file, text));
    else
        ui.placeholder = opt.file.empty() ? "waiting for the editor" : "cannot read " + opt.file;
    if (ui.placeholder.empty()) ui.placeholder = "no functions found";

    Screen screen = Screen::Create(Dimension::Fixed(width), Dimension::Fixed(height));
    Render(screen, ui.frame());
    std::string out;
    for (int y = 0; y < height; ++y) {
        std::string line;
        for (int x = 0; x < width; ++x) {
            const std::string& ch = screen.PixelAt(x, y).character;
            line += ch.empty() ? " " : ch;
        }
        while (!line.empty() && line.back() == ' ') line.pop_back();
        out += line + "\n";
    }
    return out;
}

int run_app(const AppOptions& opt) {
    auto screen = ScreenInteractive::Fullscreen();
    Ui ui;
    configure(ui, opt);
    std::atomic<bool> connected{false};
    ui.session.connection = connection_text(opt, false, ui.session.view.theme);
    ui.placeholder = opt.file.empty() ? "waiting for the editor" : "no functions found";

    // --- animation: a timer thread posts Event::Custom at ~10 fps, and only
    // while an animated arc is on screen.
    const auto started = std::chrono::steady_clock::now();
    ui.tick = [started] {
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        return (long)(ms.count() / 100);
    };
    std::mutex anim_mu;
    std::condition_variable anim_cv;
    bool anim_active = false, anim_quit = false;
    ui.on_animation = [&](bool active) {
        std::lock_guard<std::mutex> lock(anim_mu);
        if (active != anim_active) {
            anim_active = active;
            anim_cv.notify_all();
        }
    };
    std::thread anim_thread([&] {
        std::unique_lock<std::mutex> lock(anim_mu);
        while (!anim_quit) {
            anim_cv.wait(lock, [&] { return anim_quit || anim_active; });  // idle: no wakeups
            if (anim_quit) break;
            anim_cv.wait_for(lock, std::chrono::milliseconds(100), [&] { return anim_quit; });
            if (!anim_quit && anim_active) screen.PostEvent(Event::Custom);
        }
    });

    // --- sources: parsed off the UI thread, applied on it
    Updater updater([&](Model m) {
        auto shared = std::make_shared<Model>(std::move(m));
        screen.Post([&ui, shared] { ui.session.set_model(std::move(*shared)); });
        screen.PostEvent(Event::Custom);
    });
    auto load_file = [&] {
        std::string text;
        if (!opt.file.empty() && read_file(opt.file, &text)) updater.submit({opt.file, std::move(text)});
    };
    load_file();

    std::unique_ptr<FileWatcher> watcher;
    if (!opt.file.empty()) {
        watcher = start_file_watcher(opt.file, [&] {
            // while an editor is connected its buffer wins over the file on disk
            if (!connected) load_file();
        });
    }

    std::unique_ptr<Server> server;
    if (!opt.socket_path.empty()) {
        server = make_server();
        Server::Callbacks cb;
        cb.on_connect = [&] {
            connected = true;
            screen.Post([&] { ui.session.connection = connection_text(opt, true, ui.session.view.theme); });
            screen.PostEvent(Event::Custom);
        };
        cb.on_line = [&](const std::string& line) {
            Message m = parse_message(line);
            if (m.type == Message::Type::Invalid) return;
            if (m.type == Message::Type::Buffer) updater.submit({m.path, std::move(m.text)});
            if (m.has_cursor) {
                int l = m.line, c = m.col;
                screen.Post([&ui, l, c] { ui.session.set_cursor(l, c); });
            }
            screen.PostEvent(Event::Custom);
        };
        cb.on_disconnect = [&] {
            connected = false;
            screen.Post([&] {
                ui.session.connection = connection_text(opt, false, ui.session.view.theme);
                ui.session.set_cursor(0, 0);
            });
            load_file();  // back to the file on disk
            screen.PostEvent(Event::Custom);
        };
        std::string error;
        if (!server->start(opt.socket_path, cb, &error)) {
            ui.session.message = error;
            server.reset();
        }
    }

    auto jump = [&] {
        Session::Jump j = ui.session.jump_target();
        if (!j.ok) return;
        ui.session.message = j.path + ":" + std::to_string(j.line);
        if (connected && server) {
            server->send(jump_message(j.path, j.line, j.col));
        } else if (!opt.jump_cmd.empty()) {
            run_detached(expand_jump_cmd(opt.jump_cmd, j.path, j.line, j.col));
        }
    };

    auto renderer = Renderer([&] { return ui.frame(); });
    auto app = CatchEvent(renderer, [&](Event e) {
        if (e == Event::Custom) return true;  // a redraw request (animation frame or new data)
        if (e.is_mouse()) {
            const Mouse& m = e.mouse();
            if (ui.show_help) return true;
            if (m.button == Mouse::WheelUp || m.button == Mouse::WheelLeft) {
                ui.session.scroll_by(-4);
            } else if (m.button == Mouse::WheelDown || m.button == Mouse::WheelRight) {
                ui.session.scroll_by(4);
            } else if (m.button == Mouse::Left && m.motion == Mouse::Pressed) {
                if (ui.diagram_box.Contain(m.x, m.y))
                    ui.session.select_at(m.x - ui.diagram_box.x_min + ui.session.scroll_x(),
                                         m.y - ui.diagram_box.y_min + ui.session.scroll_y());
            } else {
                return false;
            }
            return true;
        }
        if (ui.show_help) {
            ui.show_help = false;
            return true;
        }
        const std::string& in = e.input();
        ui.session.message.clear();
        // Shift+arrows arrive as CSI 1;2 C/D
        if (in == "\x1b[1;2D" || e == Event::Character('[')) ui.session.scroll_by(-8);
        else if (in == "\x1b[1;2C" || e == Event::Character(']')) ui.session.scroll_by(8);
        else if (e == Event::ArrowLeft) ui.session.move(Dir::Left);
        else if (e == Event::ArrowRight) ui.session.move(Dir::Right);
        else if (e == Event::ArrowUp) ui.session.move(Dir::Up);
        else if (e == Event::ArrowDown) ui.session.move(Dir::Down);
        else if (e == Event::Return) jump();
        else if (e == Event::Character(' ')) ui.session.toggle_collapse();
        else if (e == Event::Tab) ui.session.toggle_order();
        else if (e == Event::Character('t') || e == Event::Character('T')) {
            ui.session.cycle_theme();
            ui.session.connection = connection_text(opt, connected, ui.session.view.theme);
        } else if (e == Event::Character('l') || e == Event::Character('L')) ui.session.toggle_library();
        else if (e == Event::Character('?')) ui.show_help = true;
        else if (e == Event::Character('q') || e == Event::Character('Q')) screen.Exit();
        else return false;
        return true;
    });

    screen.Loop(app);

    // stop everything that can still post to the screen before it goes away
    if (server) server->stop();
    if (watcher) watcher->stop();
    {
        std::lock_guard<std::mutex> lock(anim_mu);
        anim_quit = true;
    }
    anim_cv.notify_all();
    anim_thread.join();
    return 0;
}

}  // namespace tracespace
