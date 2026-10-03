#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace ldc {

struct CommandLine {
    std::vector<std::string> modules;
    std::string programDot;
    bool help = false;
    std::string error;
    std::string analysisDot;
    std::string sourceFile;
    std::string facts;
    std::string exportDir;
    std::string mode = "lfc";
    unsigned k = 0;
    bool p3ctx = false;
    bool compareAndersen = false;
};

CommandLine parseCommandLine(int argc, char** argv);
void printHelp(std::ostream& out);

} // namespace ldc
