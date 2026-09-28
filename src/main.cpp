// ldc — L_DC prototype driver.
//
// Loads LLVM IR through SVF, builds SVFIR, runs Andersen (its call graph gives
// the indirect / virtual call targets), and builds LDGraph from SVFIR.
//
// Options (parsed by SVF's option parser, so they coexist with SVF's own):
//   -ldc-dot=<file>     write LDGraph to a Graphviz file

#include "ldc/Builder.h"
#include "ldc/LDGraph.h"

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

    if (!DotOut().empty())
    {
        std::ofstream dot(DotOut());
        graph.dumpDot(dot);
        std::cout << "LDGraph written to " << DotOut() << "\n";
    }

    AndersenWaveDiff::releaseAndersenWaveDiff();
    SVFIR::releaseSVFIR();
    LLVMModuleSet::releaseLLVMModuleSet();
    return 0;
}
