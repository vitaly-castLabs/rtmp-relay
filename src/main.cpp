#include "rtmp_relay.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct AppConfig {
    RelayConfig relay;
    std::chrono::seconds stats_period = std::chrono::seconds(10);
};

void parse_transform_spec(const std::string& spec, RelayConfig& relay_config) {
    constexpr const char* kPrefix = "dl=";
    if (spec.rfind(kPrefix, 0) != 0)
        throw std::invalid_argument("--transform must start with dl=");

    const size_t path_start = 3;
    const size_t sep = spec.find(';', path_start);
    relay_config.transform_path = spec.substr(path_start, sep == std::string::npos ? std::string::npos : sep - path_start);
    if (relay_config.transform_path.empty())
        throw std::invalid_argument("--transform requires a non-empty dl= path");

    relay_config.transform_params = sep == std::string::npos ? "" : spec.substr(sep + 1);
}

AppConfig parse_config(int argc, char** argv) {
    AppConfig app_config;
    std::vector<std::string> positional;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--stats-period") {
            if (++i >= argc)
                throw std::invalid_argument("--stats-period requires a value");
            int secs = std::stoi(argv[i]);
            if (secs <= 0)
                throw std::invalid_argument("--stats-period must be positive");
            app_config.stats_period = std::chrono::seconds(secs);
        } else if (arg == "--transform") {
            if (++i >= argc)
                throw std::invalid_argument("--transform requires a value");
            parse_transform_spec(argv[i], app_config.relay);
        } else if (!arg.empty() && arg.front() == '-') {
            throw std::invalid_argument("unknown option: " + arg);
        } else {
            positional.push_back(arg);
        }
    }

    if (positional.size() > 2)
        throw std::invalid_argument("expected at most two RTMP URLs");

    app_config.relay.input_url = positional.size() > 0 ? positional[0] : "rtmp://0.0.0.0:19350/live/in";
    app_config.relay.output_url = positional.size() > 1 ? positional[1] : "rtmp://127.0.0.1:19351/live/out";

    return app_config;
}

void print_usage(const char* exe_name) {
    std::cerr << "Usage: " << exe_name << " [options] [input_rtmp_url] [output_rtmp_url]\n"
              << "Options:\n"
              << "  --transform <spec>           Transform spec, e.g. dl=./build/transform.so;file=out.h264\n"
              << "  --stats-period <secs>        Stats print interval (default: 10s)\n"
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
