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
    app.add_flag("--match-calls", options.matchCalls,
                 "Keep only facts with a realizable path (calls and returns match, L_C)");
    app.add_option("--facts", options.facts, "Write the points-to facts to a file")->check(path);
    app.add_option("--source", options.source, "Name objects by `// <label>` source comments")
        ->check(CLI::ExistingFile)
        ->needs("--facts");
    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) { return app.exit(error); }
    if (options.matchCalls) options.pointsTo = true;
    return options;
}

} // namespace ldc
