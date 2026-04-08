#pragma once

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>

struct RelayConfig {
    std::string input_url = "rtmp://0.0.0.0:19350/live/in";
    std::string output_url = "rtmp://127.0.0.1:19351/live/out";
};

class RtmpRelay {
public:
    explicit RtmpRelay(RelayConfig config);

    [[nodiscard]] int run();
    void request_stop();
    [[nodiscard]] bool stop_requested() const;

private:
    int relay_loop();
    bool open_input();
    bool open_output();
    bool ensure_output();
    void handle_output_disconnect(const std::string& error_text);
    void close_output();
    void close_input();
    void log_stream_map() const;

    RelayConfig config_;
    std::atomic<bool> stop_requested_ {false};
    struct AVFormatContext* input_ctx_ = nullptr;
    struct AVFormatContext* output_ctx_ = nullptr;
    int video_stream_index_ = -1;
    std::uint64_t video_frame_count_ = 0;
    bool waiting_for_video_keyframe_ = false;
    std::chrono::steady_clock::time_point next_output_retry_at_ = std::chrono::steady_clock::time_point::min();
};

class RelayApp {
public:
    explicit RelayApp(RelayConfig config);

    [[nodiscard]] int run();

private:
    void handle_stop_signal(asio::error_code ec, int signal_number);
    static void install_sigpipe_handler();

    asio::io_context io_context_;
    asio::signal_set signals_;
    RtmpRelay relay_;
    std::thread relay_thread_;
    std::optional<int> exit_code_;
};
