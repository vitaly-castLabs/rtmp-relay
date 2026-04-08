#include "rtmp_relay.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

struct AppConfig {
    RelayConfig relay;
    std::chrono::seconds stats_period = std::chrono::seconds(10);
};

AppConfig parse_config(int argc, char** argv) {
    AppConfig app_config;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--stats-period") {
            if (++i >= argc)
                throw std::invalid_argument("--stats-period requires a value");
            int secs = std::stoi(argv[i]);
            if (secs <= 0)
                throw std::invalid_argument("--stats-period must be positive");
            app_config.stats_period = std::chrono::seconds(secs);
        } else if (!arg.empty() && arg.front() == '-') {
            throw std::invalid_argument("unknown option: " + arg);
        } else {
            if (app_config.relay.input_url.empty()) {
                app_config.relay.input_url = arg;
            } else if (app_config.relay.output_url.empty()) {
                app_config.relay.output_url = arg;
            } else {
                throw std::invalid_argument("expected at most two RTMP URLs");
            }
        }
    }

    return app_config;
}

void print_usage(const char* exe_name) {
    std::cerr << "Usage: " << exe_name << " [options] [input_rtmp_url] [output_rtmp_url]\n"
              << "Options:\n"
              << "  --stats-period <secs>  Stats print interval (default: 10s)\n"
              << "Defaults:\n"
              << "  input:  rtmp://0.0.0.0:19350/live/in\n"
              << "  output: rtmp://127.0.0.1:19351/live/out\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        const AppConfig app_config = parse_config(argc, argv);
        RelayApp app(app_config.relay, app_config.stats_period);
        return app.run();
    } catch (const std::exception& ex) {
        print_usage(argv[0]);
        std::cerr << "Error: " << ex.what() << '\n';
        return 1;
    }
}
