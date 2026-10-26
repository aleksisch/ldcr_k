#include "CommandLine.h"
#include <CLI/CLI.hpp>

namespace ldc {

std::variant<CommandLine, int> parseCommandLine(int argc, char** argv) {
    CommandLine options;
    CLI::App app{"Build a simplified pointer assignment graph and print call edges.", "ldc"};
    const CLI::Validator path{
        [](const std::string& value) { return value.empty() ? "requires a path" : ""; }, "PATH"};
    app.add_option("inputs", options.modules, "LLVM IR modules (.ll or .bc)")->required();
    app.add_option("--program-dot", options.programDot, "Write the simplified PAG to a DOT file")
        ->check(path);
    app.add_flag("--points-to", options.pointsTo, "Solve points-to facts (L_D and L_R)");
    app.add_flag("--cpp-heap", options.cppHeap,
                 "Model C++ memory: objects at field offsets, heap cells (field-sensitive interior "
                 "pointers)");
    app.add_flag("--match-calls", options.matchCalls,
                 "Keep only facts with a realizable path (calls and returns match, L_C)");
    app.add_option("--facts", options.facts, "Write the points-to facts to a file")->check(path);
    std::string solver = "lagraph";
    app.add_option("--cfl-solver", solver, "CFL-reachability solver: lagraph or svf")
        ->check(CLI::IsMember({"lagraph", "svf"}));
    app.add_option("--source", options.source, "Name objects by `// <label>` source comments")
        ->check(CLI::ExistingFile)
        ->needs("--facts");
    app.add_option("--pag-dot", options.pagDot, "Write the PAG right before solving to a DOT file")
        ->check(path);
    app.add_option("--pag-dot-code", options.pagDotCode, "Show this source file in --pag-dot")
        ->check(CLI::ExistingFile)
        ->needs("--pag-dot");
    app.add_option("--path", options.paths,
                   "Find a path from:to (an object and a variable: points-to; two variables: "
                   "alias), print it and draw it in --pag-dot")
        ->allow_extra_args(false);
    app.add_option("--lang", options.languages,
                   "Languages to find each --path in, each on its own: any of D, C, R")
        ->check([](const std::string& value) {
            return !value.empty() && value.find_first_not_of("DCR") == std::string::npos
                       ? ""
                       : "expects letters of D, C, R";
        });
    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) { return app.exit(error); }
    if (options.matchCalls) options.pointsTo = true;
    if (options.cppHeap) options.pointsTo = true;
    options.svfSolver = solver == "svf";
    if (!options.pagDot.empty() || !options.paths.empty()) options.pointsTo = true;
    return options;
}

} // namespace ldc
