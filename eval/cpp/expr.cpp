// Expression interpreter: AST, visitors (double dispatch), environments, constant folding.
// Exercises: visitor accept/visit, factories returning fresh nodes, a linked environment,
// the same helper called from many contexts.

struct Value {
    long n;
    explicit Value(long v) : n(v) {}
};

struct Num;
struct Var;
struct Add;
struct Mul;
struct Let;
struct Neg;

struct Visitor {
    virtual ~Visitor() {}
    virtual Value* visitNum(Num* e) = 0;
    virtual Value* visitVar(Var* e) = 0;
    virtual Value* visitAdd(Add* e) = 0;
    virtual Value* visitMul(Mul* e) = 0;
    virtual Value* visitLet(Let* e) = 0;
    virtual Value* visitNeg(Neg* e) = 0;
};

struct Expr {
    virtual ~Expr() {}
    virtual Value* accept(Visitor* v) = 0;
    virtual Expr* clone() = 0;
    virtual int size() { return 1; }
};

struct Num : Expr {
    Value* value;
    explicit Num(Value* v) : value(v) {}
    Value* accept(Visitor* v) override { return v->visitNum(this); }
    Expr* clone() override { return new Num(value); }
};

struct Var : Expr {
    const char* name;
    explicit Var(const char* n) : name(n) {}
    Value* accept(Visitor* v) override { return v->visitVar(this); }
    Expr* clone() override { return new Var(name); }
};

struct Binary : Expr {
    Expr* lhs;
    Expr* rhs;
    Binary(Expr* l, Expr* r) : lhs(l), rhs(r) {}
    int size() override { return 1 + lhs->size() + rhs->size(); }
};

struct Add : Binary {
    Add(Expr* l, Expr* r) : Binary(l, r) {}
    Value* accept(Visitor* v) override { return v->visitAdd(this); }
    Expr* clone() override { return new Add(lhs->clone(), rhs->clone()); }
};

struct Mul : Binary {
    Mul(Expr* l, Expr* r) : Binary(l, r) {}
    Value* accept(Visitor* v) override { return v->visitMul(this); }
    Expr* clone() override { return new Mul(lhs->clone(), rhs->clone()); }
};

struct Neg : Expr {
    Expr* inner;
    explicit Neg(Expr* e) : inner(e) {}
    Value* accept(Visitor* v) override { return v->visitNeg(this); }
    Expr* clone() override { return new Neg(inner->clone()); }
    int size() override { return 1 + inner->size(); }
};

struct Let : Expr {
    const char* name;
    Expr* bound;
    Expr* body;
    Let(const char* n, Expr* b, Expr* e) : name(n), bound(b), body(e) {}
    Value* accept(Visitor* v) override { return v->visitLet(this); }
    Expr* clone() override { return new Let(name, bound->clone(), body->clone()); }
    int size() override { return 1 + bound->size() + body->size(); }
};

// ---- environment ----------------------------------------------------------------------

struct Env {
    const char* name;
    Value* value;
    Env* next;
    Env(const char* n, Value* v, Env* rest) : name(n), value(v), next(rest) {}
};

static bool same(const char* a, const char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

Value* lookup(Env* env, const char* name) {
    for (Env* e = env; e; e = e->next)
        if (same(e->name, name))
            return e->value;
    return nullptr;
}

Env* bind(Env* env, const char* name, Value* v) { return new Env(name, v, env); }

// ---- visitors -------------------------------------------------------------------------

struct Evaluator : Visitor {
    Env* env;
    explicit Evaluator(Env* e) : env(e) {}
    Value* visitNum(Num* e) override { return e->value; }
    Value* visitVar(Var* e) override { return lookup(env, e->name); }
    Value* visitAdd(Add* e) override {
        Value* a = e->lhs->accept(this);
        Value* b = e->rhs->accept(this);
        return new Value(a->n + b->n);
    }
    Value* visitMul(Mul* e) override {
        Value* a = e->lhs->accept(this);
        Value* b = e->rhs->accept(this);
        return new Value(a->n * b->n);
    }
    Value* visitNeg(Neg* e) override {
        Value* a = e->inner->accept(this);
        return new Value(-a->n);
    }
    Value* visitLet(Let* e) override {
        Value* bound = e->bound->accept(this);
        Env* saved = env;
        env = bind(env, e->name, bound);
        Value* result = e->body->accept(this);
        env = saved;
        return result;
    }
};

struct Counter : Visitor {
    Value* zero;
    long count;
    explicit Counter(Value* z) : zero(z), count(0) {}
    Value* visitNum(Num*) override { ++count; return zero; }
    Value* visitVar(Var*) override { ++count; return zero; }
    Value* visitAdd(Add* e) override { ++count; e->lhs->accept(this); return e->rhs->accept(this); }
    Value* visitMul(Mul* e) override { ++count; e->lhs->accept(this); return e->rhs->accept(this); }
    Value* visitNeg(Neg* e) override { ++count; return e->inner->accept(this); }
    Value* visitLet(Let* e) override { ++count; e->bound->accept(this); return e->body->accept(this); }
};

// Folds constants: returns the folded value if the whole subtree is constant.
struct Folder : Visitor {
    Value* visitNum(Num* e) override { return e->value; }
    Value* visitVar(Var*) override { return nullptr; }
    Value* visitAdd(Add* e) override {
        Value* a = e->lhs->accept(this);
        Value* b = e->rhs->accept(this);
        return a && b ? new Value(a->n + b->n) : nullptr;
    }
    Value* visitMul(Mul* e) override {
        Value* a = e->lhs->accept(this);
        Value* b = e->rhs->accept(this);
        return a && b ? new Value(a->n * b->n) : nullptr;
    }
    Value* visitNeg(Neg* e) override {
        Value* a = e->inner->accept(this);
        return a ? new Value(-a->n) : nullptr;
    }
    Value* visitLet(Let*) override { return nullptr; }
};

// ---- builders (called from many sites: context sensitivity matters) ---------------------

Expr* num(long n) { return new Num(new Value(n)); }
Expr* var(const char* name) { return new Var(name); }
Expr* add(Expr* a, Expr* b) { return new Add(a, b); }
Expr* mul(Expr* a, Expr* b) { return new Mul(a, b); }
Expr* neg(Expr* a) { return new Neg(a); }
Expr* let(const char* n, Expr* b, Expr* e) { return new Let(n, b, e); }

Value* run(Visitor* v, Expr* e) { return e->accept(v); }

Expr* simplify(Expr* e) {
    Folder folder;
    Value* folded = run(&folder, e);
    return folded ? new Num(folded) : e->clone();
}

long sink;

int main() {
    Value* zero = new Value(0);
    Env* globals = bind(nullptr, "x", new Value(3));
    globals = bind(globals, "y", new Value(4));

    Expr* e1 = add(mul(num(2), var("x")), neg(var("y")));
    Expr* e2 = let("z", add(num(1), num(2)), mul(var("z"), var("z")));
    Expr* e3 = simplify(add(num(5), mul(num(6), num(7))));
    Expr* e4 = simplify(e1);

    Evaluator eval(globals);
    Counter counter(zero);
    Value* r1 = run(&eval, e1);
    Value* r2 = run(&eval, e2);
    Value* r3 = run(&eval, e3);
    Value* r4 = run(&eval, e4);
    run(&counter, e2);

    sink = r1->n + r2->n + r3->n + r4->n + counter.count + e1->size() + e2->size();
    return 0;
}
