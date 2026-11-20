void load() {}
void parse() {}
void fast() {}
void slow() { parse(); }
void report() {}
int fact(int n) { if (n <= 1) return 1; return n * fact(n - 1); }
int main(int argc, char** argv) {
    load();
    if (argc > 1) { fast(); } else { slow(); report(); }
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 2; ++j) { parse(); }
        report();
    }
    if (argc == 5) report();
    fact(3);
    return 0;
}
