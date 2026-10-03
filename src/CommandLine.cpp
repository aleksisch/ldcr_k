#include "CommandLine.h"
#include <ostream>
#include <charconv>

namespace ldc {

CommandLine parseCommandLine(int argc, char** argv) {
    CommandLine result;
    bool positionalOnly = false;
    for (auto i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto equals = arg.find('=');
        const auto name = arg.substr(0, equals);
        const auto key = name.rfind("--", 0) == 0 ? name.substr(1) : name;
        auto value = [&]() -> std::string {
            std::string text;
            if (equals != std::string::npos)
                text = arg.substr(equals + 1);
            else if (i + 1 < argc && argv[i + 1][0] != '-')
                text = argv[++i];
            if (text.empty()) result.error = name + " requires a value";
            return text;
        };
        if (positionalOnly) {
            result.modules.push_back(arg);
        } else if (arg == "--") {
            positionalOnly = true;
        } else if (arg == "--help" || arg == "-help" || arg == "-h") {
            result.help = true;
        } else if (arg == "--program-dot" || arg == "-program-dot" ||
                   arg.rfind("--program-dot=", 0) == 0 || arg.rfind("-program-dot=", 0) == 0) {
            if (!result.programDot.empty()) return {.error = "duplicate option: --program-dot"};
            const auto equals = arg.find('=');
            if (equals != std::string::npos) {
                result.programDot = arg.substr(equals + 1);
            } else {
                if (i + 1 == argc || argv[i + 1][0] == '-')
                    return {.error = "--program-dot requires an output path"};
                result.programDot = argv[++i];
            }
            if (result.programDot.empty())
                return {.error = "--program-dot requires an output path"};
        } else if (key == "-ldc-dot") {
            result.analysisDot = value();
        } else if (key == "-ldc-src") {
            result.sourceFile = value();
        } else if (key == "-ldc-facts") {
            result.facts = value();
        } else if (key == "-ldc-export") {
            result.exportDir = value();
        } else if (key == "-ldc-mode") {
            result.mode = value();
            if (result.mode != "lfc" && result.mode != "kcfa" && result.mode != "ldc" &&
                result.mode != "ldcr")
                return {.error = "unknown analysis mode: " + result.mode};
        } else if (key == "-ldc-k") {
            const auto text = value();
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result.k);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
                return {.error = "--ldc-k requires a nonnegative integer"};
        } else if (key == "-ldc-p3ctx" || key == "-ldc-andersen") {
            const auto text = equals == std::string::npos ? "true" : value();
            if (text != "true" && text != "false")
                return {.error = name + " expects true or false"};
            (key == "-ldc-p3ctx" ? result.p3ctx : result.compareAndersen) = text == "true";
        } else if (!arg.empty() && arg[0] == '-') {
            return {.error = "unknown option: " + arg};
        } else {
            result.modules.push_back(arg);
        }
        if (!result.error.empty()) return result;
    }
    if (!result.help && result.modules.empty())
        return {.error = "no input files; use --help for usage"};
    return result;
}

void printHelp(std::ostream& out) {
    out << "Usage: ldc [options] <input.ll|input.bc>...\n"
           "\n"
           "Analyze pointer flow with lfc, kcfa, ldc, or ldcr.\n"
           "\n"
           "Options:\n"
           "  -h, --help             Show this help and exit\n"
           "  --program-dot PATH     Write the simplified PAG to a DOT file\n"
           "  --ldc-mode MODE        Analysis: lfc (default), kcfa, ldc, ldcr\n"
           "  --ldc-k N              Call context depth (default: 0)\n"
           "  --ldc-dot PATH         Write the analysis graph to DOT\n"
           "  --ldc-src PATH         Source file for object labels\n"
           "  --ldc-facts PATH       Write computed points-to facts\n"
           "  --ldc-export DIR       Export analysis tables and stop\n"
           "  --ldc-p3ctx            Enable selective context sensitivity\n"
           "  --ldc-andersen         Compare our results with the Andersen baseline\n"
           "  --                     Treat remaining arguments as input paths\n";
}

} // namespace ldc
