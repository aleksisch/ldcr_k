// Paper Fig. 8. Two virtual calls on the same receiver object, to different methods.
// kCFA (any k):  p -> {i1},  q -> {i2}.
// L_DC (no L_R): i1 is stored in h1's parameter field 1 at c4 and read back by n at c5
// (the dispatch excursion starts at c4 and ends at c5, violating DP-C1): p, q -> {i1, i2}.
struct I {};
struct H {
    virtual void m(I* p) { I* sp = p; (void)sp; }
    virtual void n(I* q) { I* sq = q; (void)sq; }
};

int main() {
    H* h = new H();        // h1
    I* i1 = new I();       // i1
    I* i2 = new I();       // i2
    h->m(i1);              // c4
    h->n(i2);              // c5
    return 0;
}
