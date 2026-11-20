/* Control flow in plain C: every kind of loop, branches, early exits and goto.
 *
 *   tracespace examples/control_flow.c
 *
 * Things to look for:
 *   x8, x?, xinf      literal, unknown and endless loop counts
 *   nested arcs       one lane per nesting level
 *   stacked lanes     if / else if / else, and one lane per switch case
 *   return marker     on functions that return early
 *   break, continue   markers at the right and left ends of a loop arc
 *   dashed arrow      goto and its label
 * Press L to show the library calls (printf, fopen, ...), which start hidden.
 */
#include <stdio.h>
#include <stdlib.h>

static int read_sample(FILE* in) { return fgetc(in); }
static void record(int value) { printf("%d\n", value); }
static void warn(const char* what) { fprintf(stderr, "warning: %s\n", what); }
static void calibrate(void) {}
static void flush_output(void) { fflush(stdout); }
static void release(FILE* in) { if (in) fclose(in); }

/* A loop with a literal count: x8. */
static void warm_up(void) {
    for (int i = 0; i < 8; ++i) {
        calibrate();
    }
}

/* Nested loops, a break and a continue. The outer count depends on a variable: x?. */
static int scan(FILE* in, int rows) {
    int total = 0;
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < 4; ++c) {
            int v = read_sample(in);
            if (v == EOF) break;
            if (v < 0) continue;
            record(v);
            total += v;
        }
    }
    return total;
}

/* A switch: one lane per case, with empty cases merged into the next. */
static void classify(int value) {
    switch (value) {
        case 0:
            warn("zero");
            break;
        case 1:
        case 2:
            record(value);
            break;
        default:
            record(-1);
    }
}

/* Early returns and if / else if / else. */
static int check(int value) {
    if (value < 0) {
        warn("negative");
        return -1;
    } else if (value == 0) {
        return 0;
    } else {
        record(value);
    }
    return 1;
}

/* do-while and an endless loop that leaves with break. */
static void drain(FILE* in) {
    int v;
    do {
        v = read_sample(in);
    } while (v != EOF);

    while (1) {
        if (read_sample(in) == EOF) break;
    }
}

/* The classic goto-cleanup pattern. */
static int process(const char* path) {
    FILE* in = fopen(path, "r");
    if (!in) goto fail;

    warm_up();
    if (scan(in, 3) < 0) goto fail;
    drain(in);
    release(in);
    return 0;

fail:
    warn("giving up");
    release(in);
    return -1;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        warn("no input");
        return EXIT_FAILURE;
    }
    for (int i = 1; i < argc; ++i) {
        if (process(argv[i]) != 0) continue;
        classify(i);
        check(i);
    }
    flush_output();
    return EXIT_SUCCESS;
}
