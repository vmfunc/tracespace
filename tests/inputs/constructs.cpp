#include <cstdio>
struct Shape { virtual double area() { return 0; } };
struct Circle : Shape { double area() override { return 3; } };
void step() {}
void cleanup() {}
int open_it() { return 0; }
void inner() { for (int i = 0; i < 4; ++i) { for (;;) { while (i) { step(); if (i) break; } } } }
void pick(int k, Shape* s) {
    switch (k) { case 1: step(); break; case 2: case 3: cleanup(); break; default: s->area(); }
}
int run(void (*cb)()) {
    if (open_it() < 0) goto fail;
    cb();
    printf("ok");
    while (true) { inner(); if (open_it()) continue; }
    return 0;
fail:
    cleanup();
    return -1;
}
