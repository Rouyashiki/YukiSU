#include "../src/cli_args.hpp"
#include "../src/core/json.hpp"

#include <cassert>
#include <cstdlib>

namespace ksud {
bool plugin_config_key_is_valid(const std::string&) {
    std::abort();
}
}  // namespace ksud
namespace kagami {
bool validate_config_patch(const json::Value&, std::string&) {
    std::abort();
}
}  // namespace kagami

int main() {
    ksud::CliArguments parsed;
    assert(ksud::parse_cli({"late-load", "--package-name", "com.anatdx.yukisu"}, parsed) == -1);
    assert(parsed.command == "late-load");
    assert(parsed.value("--package-name") == "com.anatdx.yukisu");
    assert(ksud::parse_cli({"late-load", "--kmi", "android16-6.12"}, parsed) == -1);
    assert(parsed.value("--kmi") == "android16-6.12");
    assert(ksud::parse_cli({"late-load", "--post-magica", "--package-name", "com.example.manager",
                            "--allow-shell"},
                           parsed) == -1);
    assert(parsed.has("--post-magica") && parsed.has("--allow-shell"));
    assert(parsed.value("--package-name") == "com.example.manager");
    assert(ksud::parse_cli({"late-load"}, parsed) == -1);
    assert(!parsed.has("--package-name"));
    assert(ksud::parse_cli({"late-load", "--package-name"}, parsed) == 2);
    assert(ksud::parse_cli({"late-load", "--package-name="}, parsed) == 2);
}
