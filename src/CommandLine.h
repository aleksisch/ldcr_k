#pragma once

#include <string>
#include <variant>
#include <vector>

namespace ldc {

struct CommandLine {
    std::vector<std::string> modules;
    std::string programDot;
};

// Parses the arguments, or prints help or an error and returns the exit code.
std::variant<CommandLine, int> parseCommandLine(int argc, char** argv);

} // namespace ldc
