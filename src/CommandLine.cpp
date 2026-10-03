#include "CommandLine.h"
#include <ostream>
#include <stdexcept>

namespace ldc {

CommandLine parseCommandLine(int argc, char** argv) {
    CommandLine result;
    bool positionalOnly = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (positionalOnly) {
            result.modules.push_back(arg);
        } else if (arg == "--") {
            positionalOnly = true;
        } else if (arg == "--help" || arg == "-help" || arg == "-h") {
            result.help = true;
        } else if (arg == "--program-dot" || arg == "-program-dot" ||
                   arg.rfind("--program-dot=", 0) == 0 || arg.rfind("-program-dot=", 0) == 0) {
            if (!result.programDot.empty())
                throw std::invalid_argument("duplicate option: --program-dot");
            const auto equals = arg.find('=');
            if (equals != std::string::npos) {
                result.programDot = arg.substr(equals + 1);
            } else {
                if (i + 1 == argc || argv[i + 1][0] == '-')
                    throw std::invalid_argument("--program-dot requires an output path");
                result.programDot = argv[++i];
            }
            if (result.programDot.empty())
                throw std::invalid_argument("--program-dot requires an output path");
        } else if (!arg.empty() && arg[0] == '-') {
            throw std::invalid_argument("unknown option: " + arg);
        } else {
            result.modules.push_back(arg);
        }
    }
    if (!result.help && result.modules.empty())
        throw std::invalid_argument("no input files; use --help for usage");
    return result;
}

void printHelp(std::ostream& out) {
    out << "Usage: ldc [options] <input.ll|input.bc>...\n"
           "\n"
           "Build a simplified pointer assignment graph and print call edges.\n"
           "\n"
           "Options:\n"
           "  -h, --help             Show this help and exit\n"
           "  --program-dot PATH     Write the simplified PAG to a DOT file\n"
           "  --                     Treat remaining arguments as input paths\n";
}

} // namespace ldc
