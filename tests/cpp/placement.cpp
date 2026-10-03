// Placement new into pool memory, and a polymorphic member object (the tinyxml2 pattern:
// XMLDocument owns MemPoolT members; nodes are built with `new (pool.Alloc()) T(...)`).
// kCFA: the pool member is a Pool (its virtual alloc() is reached through the interior
// pointer &doc->pool), and the node built at `leaf` is a Leaf.
// SVF Andersen resolves neither virtual call here (checked by placement.andersen).
#include <new>

struct Pool {
    virtual void* alloc() { return buf + 16 * used++; }
    char buf[64];
    int used = 0;
};
// A class template deriving from an abstract base (tinyxml2's MemPoolT<N> : MemPool).
struct AbstractPool { virtual void* alloc() = 0; };
template <int N> struct TPool : AbstractPool {
    void* alloc() override { return buf; }
    char buf[N];
};

struct Node { virtual Node* self() { return this; } };
struct Leaf : Node { Node* self() override { return this; } };
// Through a reference: a virtual call on the member subobject &doc->pool.
Node* build(Pool& p) { return new (p.alloc()) Leaf(); }   // leaf
Node* buildT(AbstractPool& p) { return new (p.alloc()) Leaf(); }   // tleaf

struct Doc {
    int id = 0;
    Pool pool;              // member at a non-zero offset
    TPool<32> tpool;        // member of a class-template type
    Node* make() { return build(pool); }
    Node* makeT() { return buildT(tpool); }
};

int main() {
    Doc* d = new Doc();    // doc
    Node* n = d->make();
    Node* s = n->self();
    Node* t = d->makeT();
    Node* u = t->self();
    return s != nullptr && u != nullptr;
}
