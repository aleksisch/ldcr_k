// Names — map analysis nodes back to the source program, for tests and reports.
//
// Variables: after mem2reg, `O* x = id(a)` is the SSA value %call; the name `x`
// survives only in debug info (#dbg_value / #dbg_declare). We read it from there.
// Objects: labelled by the `// <label>` comment on their allocation line.

#pragma once

#include "ldc/LDGraph.h"

#include <string>
#include <unordered_map>

namespace ldc
{

/// "E::foo(G*)" -> "E::foo".
std::string functionKey(const std::string& demangledName);

/// SVF value-node id -> "<function>::<variable>" (function as in functionKey), from the
/// debug info of all modules loaded in SVF's LLVMModuleSet.
std::unordered_map<SvfId, std::string> collectDebugNames();

/// Source line -> label, where the line contains `// <label>` (first word after //).
std::unordered_map<int, std::string> readLineLabels(const std::string& sourcePath);

} // namespace ldc
