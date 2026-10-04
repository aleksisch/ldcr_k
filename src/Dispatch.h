#pragma once

#include "ldc/SimplifiedPAG.h"

namespace SVF { class SVFIR; }

namespace ldc::frontend::detail {

// Encodes virtual calls as receiver-based dispatch (paper Fig. 6, [C-VCall]).
void buildDispatchGraph(SVF::SVFIR& pag, SimplifiedPAG& graph);

}
