// Paper Fig. 5. Two receiver objects under ONE context.
// kCFA:  this of E::foo -> {e1},  this of F::foo -> {f1}.
// L_FC (external call graph, plain assign edges): both get {e1, f1}.
struct G { void* g; };

void* sink;

struct E { virtual void foo(G* p) { sink = p ? p->g : nullptr; sink = this; } };
struct F : E { void foo(G* q) override { (void)q; sink = this; } };

int main(int argc, char**) {
    G* w = new G();                          // g1
    if (argc > 1) { E* e1 = new E(); w->g = e1; }   // e1
    else          { F* f1 = new F(); w->g = f1; }   // f1
    E* x = static_cast<E*>(w->g);
    x->foo(nullptr);                         // c (virtual)
    return 0;
}
