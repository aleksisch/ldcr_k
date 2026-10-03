#pragma once

#include "SVFDetails.h"

namespace ldc::frontend::detail {

// Analysis-specific normalization: fold memory accesses onto GEP bases,
// retaining explicit edges for addresses used as values.
SvfEdges foldGeps(SVF::SVFIR& pag, const SvfEdges& edges);

} // namespace ldc::frontend::detail
