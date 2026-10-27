// Constructors defined out of line: clang emits the complete-object constructor (C1) as an alias of
// the base-object one (C2), and SVF gives a call through the alias no callee. The objects still
// get their class, so the virtual calls on them reach Doc::get.
struct A {};
struct Doc {
    A* a;
    explicit Doc(A* a);
    virtual A* get() {
        A* r = a;
        return r;
    }
};
Doc::Doc(A* a) : a(a) {}

int main() {
    A* a1 = new A();            // a1
    A* a2 = new A();            // a2
    Doc doc(a1);                // doc
    Doc* heap = new Doc(a2);    // heap
    Doc* d = &doc;
    A* x = d->get();
    A* y = heap->get();
    (void)x, (void)y;
    return 0;
}
