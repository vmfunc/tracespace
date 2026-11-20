#!/usr/bin/env python3
"""End-to-end test of the interactive view: runs the real binary under a
pseudo-terminal, types keys and mouse events at it, and reads the screen back
through a terminal emulator (pyte).

    python3 tui_test.py /path/to/tracespace
"""
import fcntl
import json
import os
import pty
import select
import shutil
import signal
import socket
import struct
import sys
import tempfile
import termios
import time

import pyte

BINARY = sys.argv[1] if len(sys.argv) > 1 else "tracespace"

EXAMPLE = """void a1() {}
void a2() {}
void funcA() {
    for (int i = 0; i < 2; ++i) { a1(); a2(); }
}
void funcC() {}
int main() { funcA(); funcC(); return 0; }
"""

RIGHT, LEFT, UP, DOWN = "\x1b[C", "\x1b[D", "\x1b[A", "\x1b[B"
SHIFT_RIGHT, SHIFT_LEFT = "\x1b[1;2C", "\x1b[1;2D"


class Term:
    """The program under test, attached to a pty of a given size."""

    def __init__(self, args, cols=100, rows=30, env=None):
        self.cols, self.rows = cols, rows
        self.screen = pyte.Screen(cols, rows)
        self.stream = pyte.ByteStream(self.screen)
        full_env = dict(os.environ, TERM="xterm-256color", LANG="C.UTF-8", LC_ALL="C.UTF-8")
        full_env.pop("NO_COLOR", None)
        full_env.update(env or {})
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.execvpe(BINARY, [BINARY] + args, full_env)
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        os.kill(self.pid, signal.SIGWINCH)
        self.exit_code = None

    def pump(self, duration=0.05):
        end = time.time() + duration
        while True:
            left = end - time.time()
            if left <= 0:
                return
            ready, _, _ = select.select([self.fd], [], [], left)
            if not ready:
                return
            try:
                data = os.read(self.fd, 65536)
            except OSError:
                return
            if not data:
                return
            self.stream.feed(data)

    def text(self):
        return "\n".join(line.rstrip() for line in self.screen.display)

    def wait(self, what, pred=None, timeout=6.0):
        """Wait until `what` (a substring, or a predicate on the screen text) holds."""
        check = pred or (lambda t: what in t)
        end = time.time() + timeout
        while time.time() < end:
            self.pump(0.05)
            if check(self.text()):
                return
        raise AssertionError("timed out waiting for %r; screen was:\n%s" % (what, self.text()))

    def send(self, keys):
        os.write(self.fd, keys.encode())
        self.pump(0.15)

    def find(self, needle):
        """(column, row) of the first occurrence of needle on screen, 0-based."""
        for y, line in enumerate(self.screen.display):
            x = line.find(needle)
            if x >= 0:
                return x, y
        raise AssertionError("%r not on screen:\n%s" % (needle, self.text()))

    def cell(self, x, y):
        return self.screen.buffer[y][x]

    def click(self, x, y):
        # SGR mouse: press and release of the left button, 1-based coordinates
        self.send("\x1b[<0;%d;%dM\x1b[<0;%d;%dm" % (x + 1, y + 1, x + 1, y + 1))

    def wheel(self, down, x=5, y=5):
        self.send("\x1b[<%d;%d;%dM" % (65 if down else 64, x + 1, y + 1))

    def status(self):
        return self.screen.display[self.rows - 1].rstrip()

    def wait_exit(self, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            self.pump(0.05)
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                self.exit_code = os.waitstatus_to_exitcode(status)
                return self.exit_code
        raise AssertionError("program did not exit")

    def kill(self):
        if self.exit_code is None:
            try:
                os.kill(self.pid, signal.SIGKILL)
                os.waitpid(self.pid, 0)
            except OSError:
                pass


PASSED = []


def test(fn):
    workdir = tempfile.mkdtemp(prefix="tracespace-e2e-")
    terms = []

    def start(args, **kw):
        t = Term(args, **kw)
        terms.append(t)
        return t

    try:
        fn(workdir, start)
        PASSED.append(fn.__name__)
        print("ok    " + fn.__name__, flush=True)
    finally:
        for t in terms:
            t.kill()
        shutil.rmtree(workdir, ignore_errors=True)


def write(path, text):
    # save the way editors do: write a new file, then rename it over the old one
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write(text)
    os.replace(tmp, path)


def example(workdir, text=EXAMPLE, name="demo.cpp"):
    path = os.path.join(workdir, name)
    with open(path, "w") as f:
        f.write(text)
    return path


# ---------------------------------------------------------------- tests


def draws_the_diagram_and_status_bar(workdir, start):
    t = start(["--theme", "rounded", example(workdir)])
    t.wait("funcA")
    t.wait("watching file")
    text = t.text()
    assert "│ main │───▶│   funcA    │───▶│   funcC    │" in text, text
    assert "╭─── ×2 ───╮" in text, text
    assert "│ a1  ││ a2  │" in text, text
    assert t.status().startswith(" demo.cpp │ execution │ rounded │ watching file"), t.status()


def arrow_keys_move_the_selection(workdir, start):
    t = start(["--theme", "rounded", "--no-anim", example(workdir)])
    t.wait("funcA")
    x, y = t.find("main")
    assert t.cell(x, y).bold, "main should start selected"
    t.send(RIGHT)
    x, y = t.find("funcA")
    t.wait("funcA selected", lambda _: t.cell(*t.find("funcA")).bold)
    assert not t.cell(*t.find("main")).bold
    t.send(DOWN)
    t.wait("a1 selected", lambda _: t.cell(*t.find("a1")).bold)
    t.send(RIGHT)
    t.wait("a2 selected", lambda _: t.cell(*t.find("a2")).bold)
    t.send(UP)
    t.wait("funcA selected again", lambda _: t.cell(*t.find("funcA")).bold)


def selected_loop_is_heavy_and_animates(workdir, start):
    t = start(["--theme", "rounded", example(workdir)])
    t.wait("funcA")
    assert "┏" not in t.text()
    t.send(RIGHT)  # select funcA, whose body holds the loop
    t.wait("┏")
    seen = set()
    runner_positions = set()
    end = time.time() + 8
    while time.time() < end and not ({"1/2", "2/2"} <= seen and len(runner_positions) >= 4):
        t.pump(0.05)
        text = t.text()
        for label in ("1/2", "2/2"):
            if label in text:
                seen.add(label)
        if "●" in text:
            runner_positions.add(t.find("●"))
    assert {"1/2", "2/2"} <= seen, "pass counter never showed both passes: %s" % seen
    assert len(runner_positions) >= 4, "runner did not move: %s" % runner_positions
    # moving off the loop stops the animation and restores the plain arc
    t.send(RIGHT)
    t.wait("╭─── ×2 ───╮")
    assert "●" not in t.text()


def no_anim_flag_disables_animation(workdir, start):
    t = start(["--theme", "rounded", "--no-anim", example(workdir)])
    t.wait("funcA")
    t.send(RIGHT)
    t.wait("┏━━━ ×2 ━━━┓")
    t.pump(1.0)
    assert "●" not in t.text() and "1/2" not in t.text(), t.text()


def idle_program_draws_nothing(workdir, start):
    # with nothing animating, the timer must be stopped: no output at all
    t = start(["--theme", "rounded", example(workdir)])
    t.wait("funcA")
    t.pump(0.5)
    quiet = 0
    end = time.time() + 1.5
    while time.time() < end:
        ready, _, _ = select.select([t.fd], [], [], 0.1)
        if ready:
            quiet += len(os.read(t.fd, 65536))
    assert quiet == 0, "%d bytes written while idle" % quiet


def space_collapses_and_expands(workdir, start):
    t = start(["--theme", "rounded", "--no-anim", example(workdir)])
    t.wait("a1")
    t.send(RIGHT)
    t.send(" ")
    t.wait("▸ 2")
    assert "a1" not in t.text()
    t.send(" ")
    t.wait("a1")
    assert "▸" not in t.text()


def tab_switches_ordering(workdir, start):
    t = start(["--theme", "rounded", "--no-anim", example(workdir)], cols=120)
    t.wait("funcA")
    t.send("\t")
    t.wait("source")
    # source order: every definition on the top row, a1 first
    x_a1, y_a1 = t.find("a1")
    x_main, y_main = t.find("main")
    assert y_a1 == y_main and x_a1 < x_main, t.text()
    t.send("\t")
    t.wait("execution")


def t_cycles_themes(workdir, start):
    t = start(["--theme", "rounded", "--no-anim", example(workdir)])
    t.wait("╭─── ×2 ───╮")
    t.send("T")
    t.wait("ascii")
    assert ".-~~ x2 ~~-." in t.text(), t.text()
    assert "+------+" in t.text()
    t.send("T")
    t.wait("braille")
    assert any(0x2800 < ord(ch) <= 0x28FF for ch in t.text()), "no braille dots drawn"
    t.send("t")
    t.wait("rounded")


def l_toggles_library_calls(workdir, start):
    path = example(workdir, '#include <cstdio>\nvoid a() {}\nint main() { a(); printf("x"); }\n')
    t = start(["--theme", "rounded", path])
    t.wait("│     a      │")
    assert "printf" not in t.text()
    t.send("L")
    t.wait("printf")
    assert "lib" in t.status()
    t.send("l")
    t.wait("printf gone", lambda text: "printf" not in text)


def question_mark_shows_help(workdir, start):
    t = start(["--theme", "rounded", example(workdir)])
    t.wait("funcA")
    t.send("?")
    t.wait("press any key to close")
    assert "collapse / expand" in t.text()
    t.send("x")
    t.wait("help closed", lambda text: "press any key to close" not in text)
    assert "funcA" in t.text()


def enter_shows_location_and_runs_jump_cmd(workdir, start):
    marker = os.path.join(workdir, "jumped")
    t = start(["--theme", "rounded", "--no-anim", "--jump-cmd", "echo {file}:{line}:{col} > " + marker,
               example(workdir)])
    t.wait("funcA")
    t.send(RIGHT)
    t.send("\r")
    t.wait("demo.cpp:3")  # funcA is defined on line 3
    end = time.time() + 3
    while time.time() < end and not os.path.exists(marker):
        time.sleep(0.05)
    assert open(marker).read().strip().endswith("demo.cpp:3:0"), "jump command did not run"


def mouse_click_selects(workdir, start):
    t = start(["--theme", "rounded", "--no-anim", example(workdir)])
    t.wait("funcC")
    x, y = t.find("funcC")
    t.click(x + 1, y)
    t.wait("funcC selected", lambda _: t.cell(*t.find("funcC")).bold)
    t.send("\r")
    t.wait("demo.cpp:6")


def wide_diagrams_scroll_and_never_wrap(workdir, start):
    names = ["step%02d" % i for i in range(14)]
    src = "".join("void %s() {}\n" % n for n in names) + "int main() {\n" + "".join("  %s();\n" % n for n in names) + "}\n"
    t = start(["--theme", "rounded", "--no-anim", example(workdir, src)], cols=60, rows=12)
    t.wait("step00")
    assert "step05" not in t.text(), "diagram should be wider than the terminal"
    # no wrapping: every box label sits on the same single row
    rows = [y for y, line in enumerate(t.screen.display) if "step" in line]
    assert len(rows) == 1, t.text()

    t.send(SHIFT_RIGHT)
    t.wait("scrolled right", lambda text: "main" not in text.split("\n")[1])
    t.send(SHIFT_LEFT)
    t.wait("main")
    t.send("]")
    t.wait("scrolled with ]", lambda text: "main" not in text.split("\n")[1])
    t.send("[")
    t.wait("main")
    t.wheel(down=True)
    t.wheel(down=True)
    t.wait("scrolled with the wheel", lambda text: "main" not in text.split("\n")[1])
    t.wheel(down=False)
    t.wheel(down=False)
    t.wait("main")

    # walking the selection to the far end auto-scrolls to keep it visible
    for _ in range(14):
        t.send(RIGHT)
    t.wait("step13")
    assert t.cell(*t.find("step13")).bold
    assert "step00" not in t.text()


def redraws_when_the_file_is_saved(workdir, start):
    path = example(workdir)
    t = start(["--theme", "rounded", "--no-anim", path])
    t.wait("funcC")
    write(path, EXAMPLE.replace("funcC(); return 0;", "funcC(); funcZ(); return 0;").replace(
        "void funcC() {}", "void funcC() {}\nvoid funcZ() {}"))
    t.wait("funcZ")
    # plain in-place write, too
    with open(path, "w") as f:
        f.write("void solo() {}\nint main() { solo(); }\n")
    t.wait("solo")
    assert "funcA" not in t.text()


def parse_errors_are_handled_gracefully(workdir, start):
    path = example(workdir)
    t = start(["--theme", "rounded", "--no-anim", path])
    t.wait("funcC")
    # half-typed code: an unfinished call in main and a broken signature
    write(path, EXAMPLE.replace("funcA(); funcC();", "funcA(); funcC(; if (").replace("void funcA() {", "void funcA( {"))
    t.wait("parse error")
    text = t.text()
    assert "main" in text and "funcA" in text, text
    assert "⚠" in text, text
    # fixing the file clears the errors
    write(path, EXAMPLE)
    t.wait("errors cleared", lambda _: "parse error" not in t.status())
    assert "⚠" not in t.text()


def q_quits_cleanly(workdir, start):
    t = start(["--theme", "rounded", example(workdir)])
    t.wait("funcA")
    t.send("q")
    assert t.wait_exit() == 0


def falls_back_to_ascii_without_a_utf8_locale(workdir, start):
    t = start([example(workdir)], env={"LANG": "C", "LC_ALL": "C", "LC_CTYPE": "C"})
    t.wait("funcA")
    t.wait("ascii")
    assert "+------+" in t.text(), t.text()
    assert all(ord(ch) < 128 for ch in t.text()), "non-ASCII output in a C locale"
    t.send("T")  # the theme cannot be changed away from ascii
    t.pump(0.3)
    assert "ascii" in t.status()


def no_color_env_gives_monochrome(workdir, start):
    t = start(["--theme", "rounded", "--no-anim", example(workdir)], env={"NO_COLOR": "1"})
    t.wait("funcA")
    colors = {t.cell(x, y).fg for y in range(t.rows - 1) for x in range(t.cols)}
    assert colors == {"default"}, colors
    # the selection is still visible: drawn inverted
    assert t.cell(*t.find("main")).reverse


def editor_socket_end_to_end(workdir, start):
    path = example(workdir)
    sock_path = os.path.join(workdir, "ts.sock")
    t = start(["--theme", "rounded", "--no-anim", "--socket", sock_path, path])
    t.wait("funcA")
    t.wait("nvim ○ (waiting)")

    end = time.time() + 5
    while not os.path.exists(sock_path) and time.time() < end:
        time.sleep(0.02)
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.connect(sock_path)
    t.wait("nvim ●")

    def send(obj):
        client.sendall((json.dumps(obj) + "\n").encode())

    # unsaved buffer contents replace what is on disk
    edited = EXAMPLE.replace("void funcC() {}", "void funcC() {}\nvoid unsaved() {}").replace(
        "funcC(); return 0;", "funcC(); unsaved(); return 0;")
    send({"type": "buffer", "path": path, "text": edited, "cursor": [1, 0]})
    t.wait("unsaved")
    assert "unsaved" not in open(path).read()

    # while connected, a save on disk does not override the editor's buffer
    write(path, "void disk_only() {}\nint main() { disk_only(); }\n")
    t.pump(0.8)
    assert "disk_only" not in t.text() and "unsaved" in t.text(), t.text()

    # cursor inside funcA's loop: its box is highlighted and the loop drawn heavy
    send({"type": "cursor", "cursor": [4, 12]})
    t.wait("┏")
    assert t.cell(*t.find("funcA")).fg == "green", t.cell(*t.find("funcA")).fg
    send({"type": "cursor", "cursor": [1, 3]})
    t.wait("loop no longer highlighted", lambda text: "┏" not in text)

    # Enter sends a jump back instead of running a command
    t.send(RIGHT)
    t.send(RIGHT)  # funcC
    t.send("\r")
    client.settimeout(3)
    data = b""
    while b"\n" not in data:
        data += client.recv(4096)
    jump = json.loads(data.split(b"\n")[0])
    assert jump == {"type": "jump", "path": path, "line": 6, "col": 0}, jump

    # garbage on the socket is ignored
    client.sendall(b"this is not json\n{\"type\":\"nope\"}\n")
    t.pump(0.3)
    assert "unsaved" in t.text()

    # when the editor goes away, the file on disk is shown again
    client.close()
    t.wait("nvim ○ (waiting)")
    t.wait("disk_only")

    # and a new editor session can connect
    again = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    again.connect(sock_path)
    t.wait("nvim ●")
    again.close()
    t.send("q")
    assert t.wait_exit() == 0
    assert not os.path.exists(sock_path), "socket file left behind"


def socket_without_a_file_waits_for_the_editor(workdir, start):
    sock_path = os.path.join(workdir, "only.sock")
    t = start(["--theme", "rounded", "--socket", sock_path])
    t.wait("waiting for the editor")
    end = time.time() + 5
    while not os.path.exists(sock_path) and time.time() < end:
        time.sleep(0.02)
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.connect(sock_path)
    client.sendall((json.dumps({"type": "buffer", "path": "/x/typed.c", "text": "void f(void) {}\nint main(void) { f(); }\n"}) + "\n").encode())
    t.wait("typed.c")
    t.wait("│     f      │")
    client.close()


TESTS = [
    draws_the_diagram_and_status_bar,
    arrow_keys_move_the_selection,
    selected_loop_is_heavy_and_animates,
    no_anim_flag_disables_animation,
    idle_program_draws_nothing,
    space_collapses_and_expands,
    tab_switches_ordering,
    t_cycles_themes,
    l_toggles_library_calls,
    question_mark_shows_help,
    enter_shows_location_and_runs_jump_cmd,
    mouse_click_selects,
    wide_diagrams_scroll_and_never_wrap,
    redraws_when_the_file_is_saved,
    parse_errors_are_handled_gracefully,
    q_quits_cleanly,
    falls_back_to_ascii_without_a_utf8_locale,
    no_color_env_gives_monochrome,
    editor_socket_end_to_end,
    socket_without_a_file_waits_for_the_editor,
]

if __name__ == "__main__":
    only = sys.argv[2:]
    failed = []
    for fn in TESTS:
        if only and fn.__name__ not in only:
            continue
        try:
            test(fn)
        except Exception as e:  # keep going so one failure does not hide the rest
            failed.append(fn.__name__)
            print("FAIL  %s\n%s" % (fn.__name__, e), flush=True)
    print("\n%d passed, %d failed" % (len(PASSED), len(failed)))
    sys.exit(1 if failed else 0)
