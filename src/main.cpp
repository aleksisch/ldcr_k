// ldc — L_DC prototype driver.
//
// M0: load LLVM IR through SVF, build SVFIR, run Andersen.

#include "SVF-LLVM/LLVMUtil.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "Util/CommandLine.h"
#include "Util/Options.h"
#include "WPA/Andersen.h"

#include <iostream>
#include <string>
#include <vector>

using namespace SVF;

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
    AndersenWaveDiff::createAndersenWaveDiff(pag);
    std::cout << "SVFIR: " << pag->getTotalNodeNum() << " nodes\n";

    AndersenWaveDiff::releaseAndersenWaveDiff();
    SVFIR::releaseSVFIR();
    LLVMModuleSet::releaseLLVMModuleSet();
    return 0;
}
