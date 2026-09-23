#include "ldc/Names.h"

#include "SVF-LLVM/LLVMModule.h"

#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DebugProgramInstruction.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <fstream>
#include <sstream>

namespace ldc
{

std::string functionKey(const std::string& demangledName)
{
    return demangledName.substr(0, demangledName.find('('));
}

namespace
{

void record(std::unordered_map<SvfId, std::string>& names, const llvm::Value* value,
            const llvm::DILocalVariable* variable, const std::string& function)
{
    if (value == nullptr || variable == nullptr)
        return;
    SVF::LLVMModuleSet* modules = SVF::LLVMModuleSet::getLLVMModuleSet();
    if (!modules->hasValueNode(value))
        return;
    names.emplace(modules->getValueNode(value), function + "::" + variable->getName().str());
}

} // namespace

std::unordered_map<SvfId, std::string> collectDebugNames()
{
    std::unordered_map<SvfId, std::string> names;
    SVF::LLVMModuleSet* modules = SVF::LLVMModuleSet::getLLVMModuleSet();
    for (SVF::u32_t i = 0; i < modules->getModuleNum(); ++i)
    {
        for (const llvm::Function& fn : *modules->getModule(i))
        {
            const std::string function = functionKey(llvm::demangle(fn.getName().str()));
            // Formal parameters keep their IR names (-fno-discard-value-names); record them
            // too, so `A::foo::this` resolves even without a debug record.
            for (const llvm::Argument& arg : fn.args())
                if (arg.hasName() && modules->hasValueNode(&arg))
                    names.emplace(modules->getValueNode(&arg), function + "::" + arg.getName().str());
            for (const llvm::BasicBlock& block : fn)
            {
                for (const llvm::Instruction& inst : block)
                {
                    for (const llvm::DbgVariableRecord& dvr :
                         llvm::filterDbgVars(inst.getDbgRecordRange()))
                        record(names, dvr.getVariableLocationOp(0), dvr.getVariable(), function);
                }
            }
        }
    }
    return names;
}

std::unordered_map<int, std::string> readLineLabels(const std::string& sourcePath)
{
    std::unordered_map<int, std::string> labels;
    std::ifstream in(sourcePath);
    std::string line;
    for (int number = 1; std::getline(in, line); ++number)
    {
        const std::size_t comment = line.find("//");
        if (comment == std::string::npos)
            continue;
        std::istringstream words(line.substr(comment + 2));
        std::string label;
        if (words >> label)
            labels.emplace(number, label);
    }
    return labels;
}

} // namespace ldc
