// main.cpp - safetyctl 進入點：解析 + 分派。所有輸出英文。

#include "commands.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

void print_help(const char *prog)
{
    std::cout <<
        "Usage: " << prog << " <command> [options]\n"
        "\n"
        "Control utility for /dev/safety_copro.\n"
        "\n"
        "Commands:\n"
        "  status                 read ioctl stats and print counters\n"
        "  inject <kind>          inject a fault; kind is one of:\n"
        "                           heartbeat-timeout | checksum-error\n"
        "  recover                force a recovery on the co-processor\n"
        "  dump-report            print debugfs files if present, else report paths\n"
        "\n"
        "Options:\n"
        "  --device PATH          device path (default /dev/safety_copro)\n"
        "  --help                 show this help and exit\n";
}

} // namespace

int main(int argc, char **argv)
{
    std::string device = "/dev/safety_copro";
    std::vector<std::string> positional;

    for (int i = 1; i < argc; ++i) {
        std::string opt = argv[i];
        if (opt == "--help" || opt == "-h") {
            print_help(argv[0]);
            return 0;
        } else if (opt == "--device") {
            if (i + 1 >= argc) {
                std::cerr << "error: --device requires a value\n";
                return 2;
            }
            device = argv[++i];
        } else {
            positional.push_back(opt);
        }
    }

    if (positional.empty()) {
        std::cerr << "error: missing command\n";
        print_help(argv[0]);
        return 2;
    }

    const std::string &cmd = positional[0];
    if (cmd == "status") {
        return safetyctl::cmd_status(device);
    } else if (cmd == "inject") {
        if (positional.size() < 2) {
            std::cerr << "error: 'inject' requires a kind\n";
            return 2;
        }
        return safetyctl::cmd_inject(device, positional[1]);
    } else if (cmd == "recover") {
        return safetyctl::cmd_recover(device);
    } else if (cmd == "dump-report") {
        return safetyctl::cmd_dump_report(device);
    }

    std::cerr << "error: unknown command '" << cmd << "'\n";
    print_help(argv[0]);
    return 2;
}
