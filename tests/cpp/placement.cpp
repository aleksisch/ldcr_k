// Objects without an allocation of their class: one built by placement new in a pool's storage,
// one on the stack. Their constructor calls make them objects of class Leaf, so the virtual calls
// on them reach Leaf::get.
#include <new>

struct A {};
struct Node {
    virtual A* get() { return nullptr; }
};
struct Leaf : Node {
    A* a;
    explicit Leaf(A* a) : a(a) {}
    A* get() override {
        A* r = a;
        return r;
    }
};
struct Pool {
    alignas(Leaf) char slot[sizeof(Leaf)];
    void* alloc() { return slot; }
};

int main() {
    A* a1 = new A();                          // a1
    A* a2 = new A();                          // a2
    Pool* pool = new Pool();                  // pool
    Node* n = new (pool->alloc()) Leaf(a1);   // pooled
    A* x = n->get();
    Leaf leaf(a2);                            // leaf
    Node* m = &leaf;
    A* y = m->get();
    (void)x, (void)y;
    return 0;
}
