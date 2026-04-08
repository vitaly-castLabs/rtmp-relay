#include "rtmp_relay.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

RelayConfig parse_config(int argc, char** argv) {
    RelayConfig config;

    int positional_count = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (!arg.empty() && arg.front() == '-')
            throw std::invalid_argument("unknown option: " + arg);

        if (positional_count == 0) {
            config.input_url = arg;
        } else if (positional_count == 1) {
            config.output_url = arg;
        } else {
            throw std::invalid_argument("expected at most two RTMP URLs");
        }
        ++positional_count;
    }

    return config;
}

void print_usage(const char* exe_name) {
    std::cerr << "Usage: " << exe_name << " [input_rtmp_url] [output_rtmp_url]\n"
              << "Default:\n"
              << "  input:  rtmp://0.0.0.0:19350/live/in\n"
              << "  output: rtmp://127.0.0.1:19351/live/out\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        const RelayConfig config = parse_config(argc, argv);
        RelayApp app(config);
        return app.run();
    } catch (const std::exception& ex) {
        print_usage(argv[0]);
        std::cerr << "Error: " << ex.what() << '\n';
        return 1;
    }
}
