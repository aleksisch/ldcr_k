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
    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) { return app.exit(error); }
    return options;
}

} // namespace ldc
