// ldc — L_DC prototype driver.
//
// Loads LLVM IR through SVF, builds SVFIR, runs Andersen (its call graph gives
// the indirect / virtual call targets), and builds LDGraph from SVFIR.
//
// Options (parsed by SVF's option parser, so they coexist with SVF's own):
//   -ldc-dot=<file>     write LDGraph to a Graphviz file
//   -ldc-k=<n>          context depth k (call strings of length <= k)
//   -ldc-mode=<m>       lfc (baseline) | kcfa (oracle); also selects the expectation key
//   -ldc-src=<file>     C++ source, for `// label` object names
//   -ldc-expect=<json>  check the queries in this file; exit 1 on failure
//   -ldc-andersen       compare every variable's PTS with SVF Andersen (use with k = 0, lfc);
//                       every function is analysed, as Andersen does; exit 1 on mismatch

#include "ldc/Builder.h"
#include "ldc/Check.h"
#include "ldc/LDGraph.h"
#include "ldc/Solver.h"

#include "SVF-LLVM/LLVMUtil.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "Util/CommandLine.h"
#include "Util/Options.h"
#include "WPA/Andersen.h"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace SVF;

namespace
{

const Option<std::string> DotOut("ldc-dot", "Write LDGraph to this Graphviz file", "");
const Option<u32_t> ContextDepth("ldc-k", "Context depth k", 0);
const Option<std::string> Mode("ldc-mode", "Analysis: lfc | kcfa", "lfc");
const Option<std::string> SourceFile("ldc-src", "C++ source file, for object labels", "");
const Option<std::string> ExpectFile("ldc-expect", "Expected-results JSON to check", "");
const Option<bool> CompareAndersen("ldc-andersen", "Compare PTS with SVF Andersen", false);

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> modules = OptionBase::parseOptions(
        argc, argv, "ldc: L_DC prototype", "[options] <input.ll>");
    if (modules.empty())
    {
        std::cerr << "usage: ldc [options] <input.ll>\n";
        return 1;
    }

    // Same entry sequence as SVF's `wpa`. We deliberately skip
    // LLVMModuleSet::preProcessBCs(): it rewrites the input to `<name>.pre.bc`,
    // and in the svftools/svf image that write fails ("Bad file descriptor")
    // and crashes the process (SVF's own `svf-ex` crashes the same way).
    LLVMModuleSet::buildSVFModule(modules);

    SVFIRBuilder builder;
    SVFIR* pag = builder.build();
    Andersen* ander = AndersenWaveDiff::createAndersenWaveDiff(pag);

    ldc::BuildStats stats;
    ldc::LDGraph graph = ldc::buildLDGraph(*pag, *ander->getCallGraph(), stats);
    graph.printSummary(std::cout);
    stats.print(std::cout);

    ldc::SolverOptions options;
    options.k = ContextDepth();
    if (Mode() == "lfc")
        options.mode = ldc::Mode::Lfc;
    else if (Mode() == "kcfa")
        options.mode = ldc::Mode::Kcfa;
    else
    {
        std::cerr << "ldc: unknown -ldc-mode=" << Mode() << " (lfc | kcfa)\n";
        return 2;
    }
    options.allFunctionsReachable = CompareAndersen();

    ldc::Solver solver(graph, options);
    solver.solve();
    std::cout << "Solver [" << Mode() << ", k = " << options.k << "]: fixpoint after "
              << solver.iterations() << " rounds, " << solver.contextCount() << " contexts, "
              << solver.methodContextCount() << " (function, context) pairs\n";

    bool ok = true;
    if (CompareAndersen())
    {
        std::size_t mismatches =
            ldc::compareWithAndersen(graph, solver, *ander, std::cout);
        std::cout << "Andersen comparison: " << mismatches << " mismatching variables\n";
        ok &= mismatches == 0;
    }
    if (!ExpectFile().empty())
    {
        ldc::ExpectResult r = ldc::checkExpected(ExpectFile(), SourceFile(), ContextDepth(), Mode(),
                                                 graph, solver, std::cout);
        std::cout << "Expected: " << r.passed << " passed, " << r.failed << " failed, " << r.skipped
                  << " skipped (other k)\n";
        ok &= r.failed == 0;
    }

    if (!DotOut().empty())
    {
        std::ofstream dot(DotOut());
        graph.dumpDot(dot);
        std::cout << "LDGraph written to " << DotOut() << "\n";
    }

    AndersenWaveDiff::releaseAndersenWaveDiff();
    SVFIR::releaseSVFIR();
    LLVMModuleSet::releaseLLVMModuleSet();
    return ok ? 0 : 1;
}
