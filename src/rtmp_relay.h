#pragma once

#include <asio.hpp>

extern "C" {
#include <libavutil/avutil.h>
}

#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <thread>

struct RelayConfig {
    std::string input_url = "rtmp://0.0.0.0:19350/live/in";
    std::string output_url = "rtmp://127.0.0.1:19351/live/out";
};

class RtmpRelay {
public:
    explicit RtmpRelay(const RelayConfig& config, asio::io_context& io_context, std::chrono::seconds stats_period = std::chrono::seconds(10))
        : config_(config), stats_timer_(io_context), stats_period_(stats_period) {}

    [[nodiscard]] int run();
    void request_stop();
    [[nodiscard]] bool stop_requested() const;

    void start_stats_timer(std::function<void()> on_tick);
    void cancel_stats_timer();
    void print_stats();

private:
    int relay_loop();
    void relay_packets();
    bool open_input();
    bool open_output();
    bool ensure_output();
    void handle_output_disconnect(const std::string& error_text);
    void close_output();
    void close_input();
    void reset_input_state();
    void log_stream_map() const;

    RelayConfig config_;
    std::atomic<bool> stop_requested_{false};
    struct AVFormatContext* input_ctx_ = nullptr;
    struct AVFormatContext* output_ctx_ = nullptr;
    int video_stream_index_ = -1;
    int audio_stream_index_ = -1;
    asio::steady_timer stats_timer_;
    std::chrono::seconds stats_period_;

    // Counters updated by relay thread (atomics for cross-thread stats reads).
    std::atomic<std::int64_t> last_video_pts_{AV_NOPTS_VALUE};
    std::atomic<std::int64_t> last_audio_pts_{AV_NOPTS_VALUE};
    std::atomic<std::uint64_t> video_frame_count_{0};
    std::atomic<std::uint64_t> video_key_count_{0};
    std::atomic<std::uint64_t> video_bytes_{0};
    std::atomic<std::uint64_t> audio_frame_count_{0};
    std::atomic<std::uint64_t> audio_bytes_{0};

    // Snapshot values from previous print_stats() call (timer thread only).
    uint64_t prev_video_frames_{0};
    uint64_t prev_video_keys_{0};
    uint64_t prev_video_bytes_{0};
    uint64_t prev_audio_frames_{0};
    uint64_t prev_audio_bytes_{0};
    bool waiting_for_video_keyframe_ = false;
    std::chrono::steady_clock::time_point next_output_retry_at_ = std::chrono::steady_clock::time_point::min();
    bool need_video_dts_offset_ = false;
    bool need_audio_dts_offset_ = false;
    int64_t video_dts_offset_ = 0;
    int64_t audio_dts_offset_ = 0;
    int64_t last_written_video_dts_ = AV_NOPTS_VALUE;
    int64_t last_written_audio_dts_ = AV_NOPTS_VALUE;
};

class RelayApp {
public:
    explicit RelayApp(const RelayConfig& config, std::chrono::seconds stats_period = std::chrono::seconds(10))
        : signals_(io_context_, SIGINT, SIGTERM), relay_(config, io_context_, stats_period) {}

    [[nodiscard]] int run();

private:
    void handle_stop_signal(asio::error_code ec, int signal_number);
    static void install_sigpipe_handler();
    void print_stats();

    asio::io_context io_context_;
    asio::signal_set signals_;
    RtmpRelay relay_;
    std::thread relay_thread_;
    std::optional<int> exit_code_;
};
