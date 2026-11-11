"""Exercise Kagami's production mount-all branch without performing mounts."""

import argparse
from pathlib import Path
import subprocess
import tempfile


def extract_block(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


FIXTURE = r"""
#include <cassert>
#include <iostream>
#include <string>
#include "mount/backend.hpp"

static bool boot_completed;
static bool late_load;
static bool restoring;
static bool supported = true;
static bool config_valid = true;
static bool mount_ok = true;
static int mounts;

namespace ksud {
std::string getprop(const std::string& name) {
    assert(name == "sys.boot_completed");
    return boot_completed ? "1" : "0";
}
bool is_late_load() { return late_load; }
}

namespace kagami {
namespace reboot {
bool authorized() { return restoring; }
bool check_support(std::string&) { return supported; }
}

bool read_config_file(Config&, std::string&, MissingConfig policy) {
    assert(policy == MissingConfig::UseDefaults);
    return config_valid;
}

std::string json_quote(const std::string& value) { return '"' + value + '"'; }

namespace mount {
MountReport mount_all_enabled(const Config&) {
    ++mounts;
    return {mount_ok, 1, 1, "overlay", ""};
}
}

@BOOT_STATE@

int mount_all() {
    @MOUNT_BRANCH@
}
}

int main() {
    for (bool boot : {false, true}) {
        for (bool late : {false, true}) {
            boot_completed = boot;
            late_load = late;
            restoring = false;
            mounts = 0;
            const bool allowed = !boot || late;
            assert(kagami::mount_all() == (allowed ? 0 : 1));
            assert(mounts == (allowed ? 1 : 0));
        }
    }

    boot_completed = true;
    late_load = true;
    mount_ok = false;
    mounts = 0;
    assert(kagami::mount_all() == 1 && mounts == 1);
    mount_ok = true;

    restoring = true;
    supported = false;
    mounts = 0;
    assert(kagami::mount_all() == 1 && mounts == 0);
    supported = true;
    config_valid = false;
    assert(kagami::mount_all() == 1 && mounts == 0);

    boot_completed = false;
    late_load = false;
    config_valid = true;
    assert(kagami::mount_all() == 0 && mounts == 1);
    std::cout << "PASS: boot, late-load, mount failure and restore checks\n";
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    kagami = Path(__file__).resolve().parents[1] / "kagami"
    source = (kagami / "src/core/command.cpp").read_text(encoding="utf-8")
    boot_state = extract_block(source, "bool system_boot_completed()")
    branch = extract_block(source, 'if (sub == "mount-all")')
    fixture = FIXTURE.replace("@BOOT_STATE@", boot_state).replace(
        "@MOUNT_BRANCH@", branch[branch.index("{") + 1 : -1]
    )
    with tempfile.TemporaryDirectory(prefix="kagami-mount-gate-") as temporary:
        test = Path(temporary) / "mount_gate.cpp"
        binary = Path(temporary) / "mount_gate"
        test.write_text(fixture, encoding="utf-8")
        flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror"]
        if args.sanitize:
            flags.append("-fsanitize=address,undefined")
        subprocess.run(
            [args.cxx, *flags, "-I", str(kagami / "src"), "-I", str(kagami / "include"),
             str(test), "-o", str(binary)],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
