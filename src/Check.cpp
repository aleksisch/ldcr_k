#include "ldc/Check.h"

#include "ldc/Names.h"

#include "MemoryModel/PointerAnalysis.h"

#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>

#include <ostream>
#include <set>
#include <string>
#include <vector>

namespace ldc
{

namespace
{

std::string join(const std::set<std::string>& items)
{
    std::string out = "{";
    for (const std::string& item : items)
        out += (out.size() > 1 ? ", " : "") + item;
    return out + "}";
}

std::string varDisplayName(const Node& node)
{
    return functionKey(node.function) + "::" + node.name;
}

} // namespace

std::size_t compareWithAndersen(const LDGraph& graph, const Solver& solver,
                                SVF::PointerAnalysis& andersen, std::ostream& os)
{
    std::size_t mismatches = 0;
    for (NodeId n = 0; n < graph.nodes().size(); ++n)
    {
        const Node& node = graph.nodes()[n];
        if (node.kind != NodeKind::Var)
            continue;

        std::set<SvfId> ours;
        for (NodeId object : solver.pts(n))
            if (graph.nodes()[object].svfId != kNoSvfId) // construction-site objects: ours only
                ours.insert(graph.nodes()[object].svfId);

        std::set<SvfId> theirs;
        for (SVF::NodeID o : andersen.getPts(node.svfId))
        {
            const SvfId base = andersen.getBaseObjVarID(o);
            if (auto obj = graph.findSvf(base); obj && graph.nodes()[*obj].kind == NodeKind::Obj)
                theirs.insert(base);
        }

        if (ours == theirs)
            continue;
        ++mismatches;
        auto names = [&](const std::set<SvfId>& ids) {
            std::set<std::string> out;
            for (SvfId id : ids)
                out.insert(graph.nodes()[*graph.findSvf(id)].name);
            return join(out);
        };
        os << "  MISMATCH " << varDisplayName(node) << ": ours " << names(ours) << ", Andersen "
           << names(theirs) << "\n";
    }
    return mismatches;
}

ExpectResult checkExpected(const std::string& jsonPath, const std::string& sourcePath, int k,
                           const std::string& mode, const LDGraph& graph, const Solver& solver,
                           std::ostream& os)
{
    ExpectResult result;
    auto buffer = llvm::MemoryBuffer::getFile(jsonPath);
    if (!buffer)
    {
        os << "  cannot read " << jsonPath << "\n";
        ++result.failed;
        return result;
    }
    llvm::Expected<llvm::json::Value> parsed = llvm::json::parse((*buffer)->getBuffer());
    if (!parsed)
    {
        os << "  cannot parse " << jsonPath << ": " << llvm::toString(parsed.takeError()) << "\n";
        ++result.failed;
        return result;
    }
    const llvm::json::Object* root = parsed->getAsObject();
    const llvm::json::Array* queries = root ? root->getArray("queries") : nullptr;
    if (queries == nullptr)
        return result;

    const auto debugNames = collectDebugNames();
    const auto labels = readLineLabels(sourcePath);
    auto objectLabel = [&](NodeId object) {
        const Node& node = graph.nodes()[object];
        auto it = labels.find(node.line);
        return it != labels.end() ? it->second : node.name;
    };

    for (const llvm::json::Value& value : *queries)
    {
        const llvm::json::Object* query = value.getAsObject();
        if (query == nullptr)
            continue;
        const std::string var = query->getString("var").value_or("").str();
        const int64_t queryK = query->getInteger("k").value_or(-1);
        if (queryK != k)
        {
            ++result.skipped;
            continue;
        }
        const llvm::json::Array* expectedArray = query->getArray(mode);
        if (expectedArray == nullptr)
            expectedArray = query->getArray("kcfa");
        std::set<std::string> expected;
        if (expectedArray != nullptr)
            for (const llvm::json::Value& item : *expectedArray)
                if (auto text = item.getAsString())
                    expected.insert(text->str());

        std::set<std::string> actual;
        bool found = false;
        for (NodeId n = 0; n < graph.nodes().size(); ++n)
        {
            const Node& node = graph.nodes()[n];
            if (node.kind != NodeKind::Var)
                continue;
            auto dbg = debugNames.find(node.svfId);
            const bool matches = (dbg != debugNames.end() && dbg->second == var) ||
                                 varDisplayName(node) == var;
            if (!matches)
                continue;
            found = true;
            for (NodeId object : solver.pts(n))
                actual.insert(objectLabel(object));
        }

        const bool ok = found && actual == expected;
        (ok ? result.passed : result.failed)++;
        os << "  " << (ok ? "PASS " : "FAIL ") << var << " k=" << k << " [" << mode << "]: expected "
           << join(expected) << ", got " << (found ? join(actual) : std::string("<variable not found>"))
           << "\n";
    }
    return result;
}

} // namespace ldc
