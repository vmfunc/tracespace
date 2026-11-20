// A file caught half-way through an edit. It does not compile, and that is the
// point: tracespace keeps drawing what still parses.
//
//   tracespace examples/broken.cpp
//
// Things to look for:
//   warning marker on the boxes of functions that contain a parse error
//   "N parse errors" in the status bar
//   the rest of the diagram (load, render, the loop) intact

void load() {}
void render() {}
void save() {}

// an unfinished call in the middle of a loop
void update(int frames) {
    for (int i = 0; i < frames; ++i) {
        load();
        render(
    }
    save();
}

// fine
void shutdown() { save(); }

// an `if` still being typed
int main(int argc, char** argv) {
    load();
    if (argc >
    update(60);
    shutdown();
    return 0;
}
