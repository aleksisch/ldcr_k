#pragma once

#include "ldc/SimplifiedPAG.h"

#include <cstddef>
#include <ostream>
#include <string>
#include <vector>

namespace ldc {

// A path drawn over the PAG: the PAG edges it walks in order, its color, and the name of its
// steps.
struct DrawnPath {
    std::vector<std::size_t> edges;
    std::string color;
    std::string name;
};

// An edge as the tuple of its two labels in the paper (Fig. 6): (above, below), below is ĉ or č
// at a call site, boxed (⟦ĉ⟧, ⟦č⟧) for a dispatch excursion, ε if none; a call's argument i and
// result are the receiver's fields i and 0. A reversed edge has "_r" above and its brackets
// swapped below.
std::string edgeTuple(const frontend::SimplifiedPAG& pag, const frontend::Edge& edge);

// The PAG without its reversed edges, the source file `code` (if any) with line numbers at the
// top right with `legend` below it, and the paths over the PAG: an edge a path walks is drawn in
// its color, with its numbered steps. With `original`, the graph `pag` was made from with the
// same nodes, its edges that `pag` no longer has are drawn too, gray and dashed.
void writePagDot(std::ostream& out, const frontend::SimplifiedPAG& pag, const std::string& code,
                 const std::string& legend, const std::vector<DrawnPath>& paths,
                 const frontend::SimplifiedPAG* original = nullptr);

} // namespace ldc
