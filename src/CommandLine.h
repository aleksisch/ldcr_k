#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace ldc {

struct CommandLine {
    std::vector<std::string> modules;
    std::string programDot;
    bool help = false;
};

CommandLine parseCommandLine(int argc, char** argv);
void printHelp(std::ostream& out);

} // namespace ldc
