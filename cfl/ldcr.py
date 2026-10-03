#!/usr/bin/env python3
"""L_DCR_k points-to as CFL-reachability, solved by the matrix-based all-pairs solver of
Muravev & Grigorev (SOAP '25, third_party/CFPQ_PyAlgo).

Input: the directory written by `ldc -ldc-mode=ldcr [-ldc-p3ctx] -ldc-export=<dir>`.

1. Product graph G x R_k: a variable node per (variable, context), a context-insensitive
   variable (global, or dropped by P3Ctx) once in []; `this` also once per dispatch instance
   (call site, caller context); objects <O, heap context, offset>; heap cells for real fields.
2. Grammar: L_D over it (flowsto per type class; dispatch[t] only for objects of class t).
3. Outer loop: the product holds only the (function, context) instances reached so far (main,
   calls, and dispatches the solver has proven); it is solved again until nothing new is
   reached. The closure is kept between rounds: a round only propagates its new edges
   (--cold: every round from scratch, CFPQ_PyAlgo as is). Objects get every offset a gep chain can reach, up front: an offset no pointer
   reaches adds nodes but no facts.

Output: the facts `var[ ctx ] -> obj[ heap ctx ]`, as `ldc -ldc-facts` writes them.
Runs in the cfpq/py_algo image with third_party/CFPQ_PyAlgo at /app.
"""
import argparse
import sys
import time
from collections import defaultdict
from pathlib import Path

import numpy as np

sys.path.insert(0, "/app")
from cfpq_algo.all_pairs.all_cfl_pairs_reachability_impls import \
    get_all_pairs_cfl_reachability_algo  # noqa: E402
from cfpq_algo.setting.algo_settings_manager import AlgoSettingsManager  # noqa: E402
from cfpq_algo.setting.preprocessor_setting import preprocess_graph_and_grammar  # noqa: E402
from cfpq_model.cnf_grammar_template import CnfGrammarTemplate  # noqa: E402
from cfpq_model.label_decomposed_graph import LabelDecomposedGraph  # noqa: E402
from cfpq_model.label_decomposed_graph import OptimizedLabelDecomposedGraph  # noqa: E402
from cfpq_model.cnf_grammar_template import Symbol  # noqa: E402
from cfpq_algo.setting.matrix_optimizer_setting import create_matrix_optimizer  # noqa: E402
from cfpq_matrix.block.block_matrix_space_impl import BlockMatrixSpaceImpl  # noqa: E402
from cfpq_matrix.matrix_utils import complimentary_mask  # noqa: E402
from cfpq_matrix.subtractable_semiring import SubtractableSemiring  # noqa: E402
import graphblas  # noqa: E402
from graphblas.core.dtypes import BOOL  # noqa: E402
from graphblas.core.matrix import Matrix  # noqa: E402

# LDGraph.h
VAR, OBJ, RECV = 0, 1, 2
NEW, ASSIGN, STORE, LOAD, DISPATCH, GEP = range(6)
NONE, ENTER, EXIT, BOX_ENTER, BOX_EXIT = range(5)
ANY_FIELD, RET_FIELD, UNKNOWN = -2, -10, -1


def synthetic(field):
    return field <= RET_FIELD


class Program:
    def __init__(self, export):
        self.nodes = []
        for line in (export / "nodes.tsv").read_text().splitlines():
            n, kind, function, type_, limit, insensitive, text = line.split("\t")
            self.nodes.append((int(kind), function, int(type_), int(limit), insensitive == "1",
                               text))
        self.sites = []
        for line in (export / "sites.tsv").read_text().splitlines():
            _, caller, text = line.split("\t")
            self.sites.append((caller, text))
        self.subtypes = {}
        self.member_types = defaultdict(set)
        for line in (export / "subtypes.tsv").read_text().splitlines():
            obj, offset, type_ = map(int, line.split("\t"))
            self.subtypes[(obj, offset)] = type_
            self.member_types[obj].add(type_)
        # Edges by the function whose contexts run them (Solver's constructor).
        self.intra = defaultdict(list)
        self.calls = defaultdict(list)
        self.access_fields = set()
        self.gep_fields = set()
        self.offset_cache = {}
        for line in (export / "edges.tsv").read_text().splitlines():
            edge = tuple(map(int, line.split("\t")))
            src, dst, label, field, _, site, direction = edge
            if direction in (ENTER, EXIT):
                self.calls[self.sites[site][0]].append(edge)
                continue
            d, s = self.nodes[dst], self.nodes[src]
            self.intra[d[1] if d[0] == VAR and d[1] else s[1]].append(edge)
            if label in (STORE, LOAD) and field >= 0:
                self.access_fields.add(field)
            if label == GEP:
                self.gep_fields.add(field)

    def offsets(self, obj):
        """Every offset a chain of geps can reach in `obj` (static: all gep fields)."""
        if obj not in self.offset_cache:
            seen, work = {0}, [0]
            while work:
                offset = work.pop()
                for f in self.gep_fields:
                    to = self.shift(obj, offset, f)
                    if to not in seen:
                        seen.add(to)
                        work.append(to)
            self.offset_cache[obj] = seen
        return self.offset_cache[obj]

    def node_ctx(self, n, ctx):
        _, function, _, _, insensitive, _ = self.nodes[n]
        return () if not function or insensitive else ctx

    def shift(self, obj, offset, field):
        if synthetic(field):
            return field if offset == 0 else field - 1000 * (offset + 3)
        if offset == ANY_FIELD or field == ANY_FIELD:
            return ANY_FIELD
        return offset + field if offset + field < self.nodes[obj][3] else ANY_FIELD

    def receiver_types(self, obj, offset):
        if offset == ANY_FIELD:
            own = self.nodes[obj][2]
            return tuple(sorted(self.member_types[obj] | ({own} if own != UNKNOWN else set())))
        if offset == 0 and self.nodes[obj][2] != UNKNOWN:
            return (self.nodes[obj][2],)
        type_ = self.subtypes.get((obj, offset), UNKNOWN)
        return () if type_ == UNKNOWN else (type_,)

    def ctx_text(self, ctx):
        return "[" + "".join(" " + self.sites[c][1] for c in ctx) + " ]"


ROOT = ("root",)  # Ω: EX(Ω, b) iff b points to some object


def push(site, ctx, k):
    return ((site,) + ctx)[:k]


class Numbering:
    """Node ids, synthetic field keys and type classes, shared by products built in turn."""

    def __init__(self):
        self.ids = {}
        self.keys = {}  # synthetic field key (field, site, caller context) -> label index
        self.classes = {}


class Product:
    """G x R_k over the reached instances and the known object offsets."""

    def __init__(self, program, k, reached, offsets, numbering=None):
        self.p, self.k = program, k
        numbering = numbering or Numbering()
        self.ids, self.keys, self.classes = numbering.ids, numbering.keys, numbering.classes
        self.edges = set()
        self.tags = defaultdict(set)  # plain `this` node key -> dispatch instances
        self.dispatches = []  # (receiver key, type, callee instance)
        self.objects = set()
        self.dispatch_types = set()
        self.shift_classes = set()
        self.news = []  # (object key, variable key)
        self.build(reached, offsets)

    def id(self, key):
        return self.ids.setdefault(key, len(self.ids))

    def var(self, n, ctx):
        return ("v", n, self.p.node_ctx(n, ctx))

    def nodes_of(self, key):
        return [key] + [("t",) + key[1:] + tag for tag in sorted(self.tags.get(key, ()))]

    def class_of(self, obj, offset):
        types = self.p.receiver_types(obj, offset)
        return self.classes.setdefault(types, len(self.classes))

    def emit(self, src, dst, label, index=None):
        self.edges.add((src, dst, label, index))

    def build(self, reached, offsets):
        p, k = self.p, self.k
        # Dispatch instances first: they split `this` before its edges are emitted.
        for function, ctx in reached:
            for src, dst, label, _, type_, site, _ in p.calls[function]:
                if label == DISPATCH:
                    self.tags[self.var(dst, push(site, ctx, k))].add((site, ctx))
        copies = []
        for function, ctx in reached:
            for src, dst, label, field, type_, site, direction in p.intra[function]:
                s, d = self.var(src, ctx), self.var(dst, ctx)
                if label == NEW:
                    heap = () if not p.nodes[dst][1] or p.nodes[src][4] else ctx[:max(k - 1, 0)]
                    offsets[(src, heap)] |= p.offsets(src)
                    self.news.append((("o", src, heap, 0), d))
                elif label == ASSIGN:
                    copies.append((s, d, "a", None))
                elif label == GEP:
                    copies.append((s, d, "gp", field + 2))  # `*` (-2) -> 0
                elif label in (STORE, LOAD):
                    value, base = (s, d) if label == STORE else (d, s)
                    prefix = "st" if label == STORE else "ld"
                    if not synthetic(field):
                        self.access(label, value, base,
                                    prefix + "S" if field == ANY_FIELD else prefix,
                                    None if field == ANY_FIELD else field)
                    elif direction in (BOX_ENTER, BOX_EXIT):  # caller side: this instance
                        self.access(label, value, base, "s" + prefix, (field, site, ctx))
                    else:  # callee side: the instance `this` was dispatched in
                        for tag in sorted(self.tags.get(base, ())):
                            self.access(label, value, ("t",) + base[1:] + tag, "s" + prefix,
                                        (field,) + tag, split_base=False)
            for src, dst, label, _, type_, site, direction in p.calls[function]:
                callee = push(site, ctx, k)
                if label == DISPATCH:
                    receiver, this = self.var(src, ctx), self.var(dst, callee)
                    self.dispatches.append((receiver, type_, (p.nodes[dst][1], callee)))
                    self.dispatch_types.add(type_)
                    for r in self.nodes_of(receiver):
                        self.emit(r, ("t",) + this[1:] + (site, ctx), "dsT%d" % type_)
                elif direction == ENTER:
                    copies.append((self.var(src, ctx), self.var(dst, callee), "a", None))
                else:
                    copies.append((self.var(src, callee), self.var(dst, ctx), "a", None))
        for s, d, label, index in copies:
            for src in self.nodes_of(s):
                self.emit(src, d, label, index)
        self.heap(offsets)

    def access(self, label, value, base, name, key, split_base=True):
        """store: value -> base; load: base -> value. Synthetic keys become label indices."""
        index = key if key is None or isinstance(key, int) else self.key(key)
        bases = self.nodes_of(base) if split_base else [base]
        if label == STORE:
            for v in self.nodes_of(value):
                for b in bases:
                    self.emit(v, b, name, index)
        else:
            for b in bases:
                self.emit(b, value, name, index)

    def key(self, key):
        return self.keys.setdefault(key, len(self.keys))

    def heap(self, offsets):
        """Objects, gep shifts <O,a+f> -> <O,a>, and the cells of real fields."""
        p = self.p
        for (obj, heap), known in offsets.items():
            for offset in known:
                self.objects.add(("o", obj, heap, offset))
                for f in p.gep_fields:
                    to = p.shift(obj, offset, f)
                    if to in known:
                        cy, cx = self.class_of(obj, to), self.class_of(obj, offset)
                        self.emit(("o", obj, heap, to), ("o", obj, heap, offset),
                                  "shC%dx%d" % (cy, cx), f + 2)
                        self.shift_classes.add((cy, cx))
            star, hub = ("c", obj, heap, ANY_FIELD), ("h", obj, heap)
            cells = {star}
            for offset in known:
                x = ("o", obj, heap, offset)
                for g in p.access_fields:
                    cell = p.shift(obj, offset, g)
                    cells.add(("c", obj, heap, cell))
                    self.emit(("c", obj, heap, cell), x, "cs", g)
                    if cell != ANY_FIELD:
                        self.emit(star, x, "cst", g)
                    else:
                        self.emit(hub, x, "chx", g)
                self.emit(star, x, "csS")
                self.emit(hub, x, "ch")
            for cell in cells:
                self.emit(cell, hub, "cu")
        for obj in self.objects:
            self.emit(ROOT, obj, "anyo")
        for obj_key, var in self.news:
            self.emit(obj_key, var, "newC%d" % self.class_of(obj_key[1], obj_key[3]))

    def write(self, directory):
        self.write_graph(directory)
        self.write_grammar(directory)

    def write_graph(self, directory):
        with open(directory / "graph.g", "w") as out:
            for src, dst, label, index in self.edges:
                a, b = self.id(src), self.id(dst)
                if index is None:
                    out.write("%d\t%d\t%s\n%d\t%d\t%s_r\n" % (a, b, label, b, a, label))
                else:
                    out.write("%d\t%d\t%s_i\t%d\n%d\t%d\t%s_r_i\t%d\n"
                              % (a, b, label, index, b, a, label, index))

    def write_grammar(self, directory):
        rules = []
        for types, c in self.classes.items():
            ft, pt = "FT%d" % c, "PT%d" % c
            rules += [(ft, "newC%d" % c), (pt, "newC%d_r" % c), (ft, ft, "a"), (pt, "a_r", pt),
                      (ft, ft, "HEAP"), (pt, "HEAPR", pt), ("PTANY", pt),
                      ("FTANY", ft), ("S", ft)]
            for t in types:
                if t in self.dispatch_types:
                    rules += [(ft, ft, "dsT%d" % t), (pt, "dsT%d_r" % t, pt)]
        # Interior pointers: <O,a+f> reaches w if <O,a> reaches a gep source s --gp_f--> w.
        # GPO / GPR join FT with the gep edge first, so they exist only at geps.
        for cx in sorted({cx for _, cx in self.shift_classes}):
            rules += [("GPO%d_i" % cx, "FT%d" % cx, "gp_i"),
                      ("GPR%d_i" % cx, "gp_r_i", "PT%d" % cx)]
        for cy, cx in self.shift_classes:
            pair = "%dx%d" % (cy, cx)
            rules += [("FT%d" % cy, "shC%s_i" % pair, "GPO%d_i" % cx),
                      ("PT%d" % cy, "GPR%d_i" % cx, "shC%s_r_i" % pair)]
        with open(directory / "grammar.cnf", "w") as out:
            for rule in rules:
                out.write("\t".join(rule) + "\n")
            out.write(HEAP_RULES + "\nCount:\nS\n")


# Real fields go through heap cells, one per (object, offset), so that pointers to different
# offsets of one object meet in the same cell; a cell's hub stands for all its object's cells
# (a load of a collapsed field, load[*]). A synthetic field (p_i, ret) of a dispatch instance
# has one cell per instance, whatever the receiver object (Solver::cellKey): a store and a load
# with the same key meet when both bases point to something, v -sst-> b -EX_r-> Ω -EX-> b' -sld-> w.
#
# CNF splits each path into binary steps, and every step is a relation the solver stores in
# full. Each rule below joins the store, load or gep edge first (STO = st · PTANY, LDO = FTANY ·
# ld, ...), so an intermediate relation exists only for the bases of real stores and loads,
# never for every variable. Every rule of HEAP has its mirror in HEAPR (PT needs the reverse).
HEAP_RULES = """HEAP\tSTCELL\tLDCELL
STCELL\tSTO_i\tcs_r_i
STO_i\tst_i\tPTANY
STCELL\tSTOS\tcsS_r
STOS\tstS\tPTANY
LDCELL\tcs_i\tLDO_i
LDCELL\tcst_i\tLDO_i
LDCELL\tcu\tHUBLD
HUBLD\tchx_i\tLDO_i
HUBLD\tch\tLDOS
LDO_i\tFTANY\tld_i
LDOS\tFTANY\tldS
HEAPR\tLDCELLR\tSTCELLR
STCELLR\tcs_i\tSTOR_i
STOR_i\tFTANY\tst_r_i
STCELLR\tcsS\tSTORS
STORS\tFTANY\tstS_r
LDCELLR\tLDOR_i\tcs_r_i
LDCELLR\tLDOR_i\tcst_r_i
LDCELLR\tHUBLDR\tcu_r
HUBLDR\tLDOR_i\tchx_r_i
HUBLDR\tLDORS\tch_r
LDOR_i\tld_r_i\tPTANY
LDORS\tldS_r\tPTANY
EX\tanyo\tFTANY
EXR\tPTANY\tanyo_r
HEAP\tSST_i\tSLD_i
SST_i\tsst_i\tEXR
SLD_i\tEX\tsld_i
HEAPR\tSLDR_i\tSSTR_i
SLDR_i\tsld_r_i\tEXR
SSTR_i\tEX\tsst_r_i
"""


def solve(directory, algorithm):
    parser = argparse.ArgumentParser()
    manager = AlgoSettingsManager()
    manager.add_args(parser)
    settings = manager.read_args(parser.parse_args([]))
    graph = LabelDecomposedGraph.read_from_pocr_graph_file(str(directory / "graph.g"))
    grammar = CnfGrammarTemplate.read_from_pocr_cnf_file(str(directory / "grammar.cnf"))
    graph, grammar = preprocess_graph_and_grammar(graph, grammar, settings)
    start = time.time()
    result = get_all_pairs_cfl_reachability_algo(algorithm).solve(
        graph=graph, grammar=grammar, settings=settings)
    seconds = time.time() - start
    rows, cols, _ = result.to_coo()  # (object, node) pairs
    return rows, cols, seconds


class WarmSolver:
    """The incremental matrix algorithm of CFPQ_PyAlgo, kept alive between rounds: the closure
    of the previous rounds stays, and a round's new edges enter as the first front (semi-naive:
    every new pair uses at least one new fact). Vertex ids and label indices are fixed up front."""

    def __init__(self, grammar_path, vertices, blocks):
        parser = argparse.ArgumentParser()
        manager = AlgoSettingsManager()
        manager.add_args(parser)
        settings = manager.read_args(parser.parse_args([]))
        self.grammar = CnfGrammarTemplate.read_from_pocr_cnf_file(str(grammar_path))
        self.structure = SubtractableSemiring(one=True, semiring=graphblas.semiring.any_pair,
                                              sub_op=complimentary_mask)
        self.vertices, self.space = vertices, BlockMatrixSpaceImpl(n=vertices, block_count=blocks)
        self.closure = OptimizedLabelDecomposedGraph(
            vertex_count=vertices, block_matrix_space=self.space, dtype=BOOL,
            matrix_optimizer=create_matrix_optimizer(settings))
        self.seen = set()

    def add(self, edges, ids):
        """Adds the edges not seen before and completes the closure; returns S as (rows, cols)."""
        coo = defaultdict(lambda: ([], []))
        for edge in edges:
            if edge in self.seen:
                continue
            self.seen.add(edge)
            src, dst, label, index = edge
            a, b = ids[src], ids[dst]
            shift = 0 if index is None else index * self.vertices
            suffix = "" if index is None else "_i"
            for name, row, col in ((label + suffix, a, b), (label + "_r" + suffix, b, a)):
                coo[name][0].append(row + shift)
                coo[name][1].append(col)
        matrices = {}
        for name, (rows, cols) in coo.items():
            symbol = Symbol(name)
            matrices[symbol] = Matrix.from_coo(
                rows, cols, True, dtype=BOOL,
                nrows=self.space.block_count * self.vertices if symbol.is_indexed else self.vertices,
                ncols=self.vertices)
        delta = LabelDecomposedGraph(self.vertices, self.space, BOOL, matrices)
        first = self.closure.empty_copy()
        first.iadd(delta, op=self.structure.semiring.monoid)
        for lhs, rhs in self.grammar.simple_rules:
            if rhs in matrices:
                first.iadd_by_symbol(lhs, matrices[rhs], op=self.structure.semiring.monoid)
        front = self.closure.rsub(first.to_unoptimized(), op=self.structure.sub_op)
        # compute_transitive_closure of IncrementalAllPairsCFLReachabilityMatrixAlgoInstance,
        # starting from the closure instead of an empty graph
        op, monoid = self.structure.semiring, self.structure.semiring.monoid
        while front.nvals != 0:
            new_front = self.closure.mxm(front, self.grammar, op=op)
            self.closure.iadd(front, op=monoid)
            self.closure.rmxm(front, self.grammar, accum=new_front, op=op)
            for lhs, rhs in self.grammar.simple_rules:
                if rhs in self.grammar.non_terminals:
                    new_front.iadd_by_symbol(lhs, front[rhs], op=monoid)
            front = self.closure.rsub(new_front.to_unoptimized(), op=self.structure.sub_op)
        rows, cols, _ = self.closure[self.grammar.start_nonterm].to_coo()
        return rows, cols


def close_calls(program, k, reached, dispatches):
    """Adds the instances that calls reach from `reached` (dispatch edges too if `dispatches`)."""
    work = list(reached)
    while work:
        function, ctx = work.pop()
        for src, dst, label, _, _, site, direction in program.calls[function]:
            instance = (program.nodes[dst][1], push(site, ctx, k))
            if (dispatches or label != DISPATCH) and direction == ENTER and \
                    instance not in reached:
                reached.add(instance)
                work.append(instance)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("export", type=Path, help="directory written by ldc -ldc-export")
    parser.add_argument("-k", type=int, required=True)
    parser.add_argument("--facts", type=Path, required=True)
    parser.add_argument("--cold", action="store_true",
                        help="solve every round from scratch (CFPQ_PyAlgo as is)")
    parser.add_argument("--algorithm", default="IncrementalAllPairsCFLReachabilityMatrix")
    args = parser.parse_args()
    program = Program(args.export)
    k = args.k

    numbering = Numbering()
    warm = None
    if not args.cold:
        # Upper bound: every instance a dispatch could reach (CHA). It fixes the node ids,
        # the label indices and the grammar; the rounds below use a subset of it.
        start = time.time()
        everything = {("", ()), ("main", ())}
        close_calls(program, k, everything, dispatches=True)
        bound = Product(program, k, everything, defaultdict(set), numbering)
        for src, dst, _, _ in bound.edges:
            bound.id(src)
            bound.id(dst)
        bound.write_grammar(args.export)
        blocks = 1 + max((index for _, _, _, index in bound.edges if index is not None), default=0)
        warm = WarmSolver(args.export / "grammar.cnf", len(numbering.ids), blocks)
        print("bound: %d instances, %d nodes, %d label indices, %.2f s"
              % (len(everything), len(numbering.ids), blocks, time.time() - start), flush=True)

    reached = {("", ()), ("main", ())}
    offsets = defaultdict(set)
    solve_time = 0.0
    for iteration in range(1, 10 ** 6):
        # Calls reach their callees in the pushed context (dispatches wait for the solver).
        close_calls(program, k, reached, dispatches=False)
        if warm:
            size = len(numbering.ids)
            product = Product(program, k, reached, offsets, numbering)
            for src, dst, _, _ in product.edges:
                product.id(src)
                product.id(dst)
            assert len(numbering.ids) == size, "a node outside the bound"
            start = time.time()
            rows, cols = warm.add(product.edges, numbering.ids)
            seconds = time.time() - start
        else:
            product = Product(program, k, reached, offsets)
            product.write(args.export)
            rows, cols, seconds = solve(args.export, args.algorithm)
        solve_time += seconds
        keys = [None] * len(product.ids)
        for key, i in product.ids.items():
            keys[i] = key
        print("iteration %d: %d instances, %d nodes, %d edges, %d pairs, %.2f s"
              % (iteration, len(reached), len(keys), 2 * len(product.edges), len(rows), seconds),
              flush=True)
        # Dispatches the solver proved: a receiver copy points to an object of the type.
        receivers = {product.ids[r] for receiver, _, _ in product.dispatches
                     for r in product.nodes_of(receiver) if r in product.ids}
        at = np.isin(cols, list(receivers))
        objects_at = defaultdict(set)
        for o, x in zip(rows[at].tolist(), cols[at].tolist()):
            objects_at[x].add(o)
        grown = False
        for receiver, type_, instance in product.dispatches:
            if instance not in reached and any(
                    type_ in program.receiver_types(keys[o][1], keys[o][3])
                    for r in product.nodes_of(receiver) if r in product.ids
                    for o in objects_at[product.ids[r]]):
                reached.add(instance)
                grown = True
        if not grown:
            break

    # Facts: (variable, context) -> (object, heap context); tags and offsets dropped.
    texts = {}
    code = np.full(len(keys), -1, dtype=np.int64)
    for i, key in enumerate(keys):
        if key[0] in ("v", "t") and program.nodes[key[1]][0] == VAR:
            text = program.nodes[key[1]][5] + program.ctx_text(key[2])
        elif key[0] == "o":
            text = program.nodes[key[1]][5] + program.ctx_text(key[2])
        else:
            continue
        code[i] = texts.setdefault(text, len(texts))
    var, obj = code[cols], code[rows]
    keep = (var >= 0) & (obj >= 0)
    facts = np.unique(var[keep] * len(texts) + obj[keep])
    names = [None] * len(texts)
    for text, i in texts.items():
        names[i] = text
    lines = sorted(names[f // len(texts)] + " -> " + names[f % len(texts)]
                   for f in facts.tolist())
    args.facts.write_text("".join(line + "\n" for line in lines))
    print("CFL [ldcr, k = %d, %s]: %d iterations, solver %.2f s, %d instances, %d facts"
          % (k, "cold" if args.cold else "warm", iteration, solve_time, len(reached),
             len(lines)))


if __name__ == "__main__":
    main()
