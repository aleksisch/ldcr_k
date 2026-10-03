// Paper Fig. 3. Two call sites of bar with different receivers; field d.f.
// C exists (never_called) but no C object reaches x.
// kCFA, k = 2:  v -> {o1} under [c3, c1];  C::foo is never a target at c3.
struct O {};
struct D { O* f; };

O* sink;

struct A { virtual void foo(D* p) { O* v = p->f; sink = v; } };
struct B : A { void foo(D* q) override { (void)q; } };
struct C : A { void foo(D* r) override { (void)r; } };

void bar(A* x, O* o) {
    D* d = new D();        // d1
    d->f = o;
    x->foo(d);             // c3 (virtual)
}

A* never_called() { return new C(); }

int main() {
    O* o1 = new O();       // o1
    O* o2 = new O();       // o2
    A* a = new A();        // a1
    B* b = new B();        // b1
    bar(a, o1);            // c1
    bar(b, o2);            // c2
    return sink != nullptr;
}
