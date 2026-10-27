#pragma once

#include "ldc/Cfl.h"
#include "ldc/SimplifiedPAG.h"

// The grammars, as cfl::Grammar for cfl::solve. Each starts at S.
namespace ldc::grammar {

// L_D: fields and typed dispatch over the PAG's terminals (Cfl.h). S derives (o, x) if
// object o flows to node x. Its rules repeat per type of allocation and per type dispatched on in
// `pag`.
cfl::Grammar ld(const frontend::SimplifiedPAG& pag);

// L_D over addObjectFields' PAG (--cpp-heap): interior pointers field-sensitive through objects
// at offsets, and stores and loads meeting in heap cells.
cfl::Grammar ldCpp(const frontend::SimplifiedPAG& pag);

// L_C: calls and returns as parentheses of their call site, over the PAG's terminals
// (Cfl.h). S derives (u, v) if a value flows from u to v along a realizable path.
cfl::Grammar lc();

// L_R: dispatch excursions, over the PAG's terminals (Cfl.h). S derives (u, v) if
// every dispatch excursion on a path from u to v closes at the site it opened at and returns to the
// calling context it left.
cfl::Grammar lr();

} // namespace ldc::grammar
