#include "rtmp_relay.h"

#include "logger.h"

extern "C" {
#include <libavcodec/codec_desc.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

#include <cerrno>
#include <csignal>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <thread>
#include <utility>

namespace {

constexpr auto kOutputRetryDelay = std::chrono::seconds(1);

std::string av_error_string(int errnum) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errnum, buffer, sizeof(buffer));
    return buffer;
}

std::string format_hms(double seconds) {
    if (seconds < 0) seconds = 0;
    const auto total_ms = static_cast<int64_t>(seconds * 1000 + 0.5);
    const int h = static_cast<int>(total_ms / 3600000);
    const int m = static_cast<int>((total_ms / 60000) % 60);
    const int s = static_cast<int>((total_ms / 1000) % 60);
    const int ms = static_cast<int>(total_ms % 1000);
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << h << ':' << std::setw(2) << m << ':' << std::setw(2) << s << '.' << std::setw(3) << ms;
    return oss.str();
}

double pts_to_seconds(int64_t pts, AVRational time_base) {
    return static_cast<double>(pts) * av_q2d(time_base);
}

uint64_t bitrate_kbps(uint64_t bytes, double seconds) {
    if (seconds <= 0) return 0;
    return static_cast<uint64_t>(static_cast<double>(bytes) * 8.0 / 1000.0 / seconds + 0.5);
}

int stop_aware_interrupt(void* opaque) {
    const auto* relay = static_cast<const RtmpRelay*>(opaque);
    return relay != nullptr && relay->stop_requested() ? 1 : 0;
}

} // namespace

int RtmpRelay::run() {
    const int network_init = avformat_network_init();
    if (network_init < 0) {
        LOG << "avformat_network_init() failed: " << av_error_string(network_init);
        return 1;
    }

    const int rc = relay_loop();
    close_output();
    close_input();
    avformat_network_deinit();
    return rc;
}

void RtmpRelay::request_stop() {
    stop_requested_.store(true, std::memory_order_relaxed);
}

bool RtmpRelay::stop_requested() const {
    return stop_requested_.load(std::memory_order_relaxed);
}

int RtmpRelay::relay_loop() {
    LOG << "Relay started: " << config_.input_url << " -> " << config_.output_url;
    start_stats_timer([this] { print_stats(); });

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        if (!open_input()) {
            if (stop_requested_.load(std::memory_order_relaxed))
                break;
            return 1;
        }

        log_stream_map();

        if (output_ctx_ == nullptr && !open_output())
            LOG << "Output is not available yet; relay will keep ingesting and retry";

        relay_packets();

        LOG << "Input disconnected, waiting for new publisher";
        close_input();
        reset_input_state();
    }

    LOG << "Stop requested";
    if (output_ctx_ != nullptr)
        av_write_trailer(output_ctx_);
    return 0;
}

void RtmpRelay::relay_packets() {
    AVPacket packet{};

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        const int read_rc = av_read_frame(input_ctx_, &packet);
        if (read_rc == AVERROR_EOF) {
            LOG << "Input stream ended (EOF)";
            return;
        }
        if (read_rc == AVERROR(EIO) &&
            (video_frame_count_.load(std::memory_order_relaxed) > 0 || audio_frame_count_.load(std::memory_order_relaxed) > 0)) {
            LOG << "Input stream closed by peer";
            return;
        }
        if (read_rc < 0) {
            if (stop_requested_.load(std::memory_order_relaxed) && read_rc == AVERROR_EXIT) {
                LOG << "Relay stopped while waiting for input";
                return;
            }

            LOG << "av_read_frame() failed: " << av_error_string(read_rc);
            return;
        }

        AVStream* input_stream = input_ctx_->streams[packet.stream_index];

        if (packet.stream_index == video_stream_index_) {
            video_frame_count_.fetch_add(1, std::memory_order_relaxed);
            if ((packet.flags & AV_PKT_FLAG_KEY) != 0)
                video_key_count_.fetch_add(1, std::memory_order_relaxed);
            video_bytes_.fetch_add(packet.size, std::memory_order_relaxed);
            last_video_pts_.store(packet.pts, std::memory_order_relaxed);
        } else if (packet.stream_index == audio_stream_index_) {
            audio_frame_count_.fetch_add(1, std::memory_order_relaxed);
            audio_bytes_.fetch_add(packet.size, std::memory_order_relaxed);
            last_audio_pts_.store(packet.pts, std::memory_order_relaxed);
        }

        if (!ensure_output()) {
            av_packet_unref(&packet);
            continue;
        }

        if (waiting_for_video_keyframe_ && video_stream_index_ >= 0) {
            const bool is_video_packet = packet.stream_index == video_stream_index_;
            const bool is_keyframe = (packet.flags & AV_PKT_FLAG_KEY) != 0;
            if (!is_video_packet || !is_keyframe) {
                av_packet_unref(&packet);
                continue;
            }

            waiting_for_video_keyframe_ = false;
            LOG << "Resumed output on video keyframe #" << video_frame_count_.load(std::memory_order_relaxed);
        }

        AVStream* output_stream = output_ctx_->streams[packet.stream_index];
        av_packet_rescale_ts(&packet, input_stream->time_base, output_stream->time_base);
        packet.pos = -1;

        // On reconnect a new publisher starts its timestamps from 0, which would
        // make the output muxer see backwards DTS and drop/corrupt packets.
        // Compute a per-stream offset so timestamps continue monotonically.
        if (packet.stream_index == video_stream_index_) {
            if (need_video_dts_offset_ && packet.dts != AV_NOPTS_VALUE) {
                video_dts_offset_ = last_written_video_dts_ + 1 - packet.dts;
                need_video_dts_offset_ = false;
                LOG << "Applying video DTS offset: " << video_dts_offset_;
            }
            if (packet.pts != AV_NOPTS_VALUE) packet.pts += video_dts_offset_;
            if (packet.dts != AV_NOPTS_VALUE) packet.dts += video_dts_offset_;
        } else if (packet.stream_index == audio_stream_index_) {
            if (need_audio_dts_offset_ && packet.dts != AV_NOPTS_VALUE) {
                audio_dts_offset_ = last_written_audio_dts_ + 1 - packet.dts;
                need_audio_dts_offset_ = false;
                LOG << "Applying audio DTS offset: " << audio_dts_offset_;
            }
            if (packet.pts != AV_NOPTS_VALUE) packet.pts += audio_dts_offset_;
            if (packet.dts != AV_NOPTS_VALUE) packet.dts += audio_dts_offset_;
        }

        // Save before av_interleaved_write_frame: it calls av_packet_move_ref
        // internally, which zeroes out packet fields (including dts) before return.
        const int pkt_stream = packet.stream_index;
        const int64_t pkt_dts = packet.dts;

        const int write_rc = av_interleaved_write_frame(output_ctx_, &packet);

        if (write_rc == 0) {
            if (pkt_stream == video_stream_index_ && pkt_dts != AV_NOPTS_VALUE)
                last_written_video_dts_ = pkt_dts;
            else if (pkt_stream == audio_stream_index_ && pkt_dts != AV_NOPTS_VALUE)
                last_written_audio_dts_ = pkt_dts;
        }

        av_packet_unref(&packet);
        if (write_rc < 0) {
            if (stop_requested_.load(std::memory_order_relaxed) && write_rc == AVERROR_EXIT) {
                LOG << "Relay stopped while writing output";
                return;
            }

            handle_output_disconnect(av_error_string(write_rc));
        }
    }
}

bool RtmpRelay::open_input() {
    input_ctx_ = avformat_alloc_context();
    if (input_ctx_ == nullptr) {
        LOG << "avformat_alloc_context() failed for input";
        return false;
    }

    input_ctx_->interrupt_callback = {stop_aware_interrupt, this};

    AVDictionary* options = nullptr;
    av_dict_set(&options, "listen", "1", 0);
    LOG << "Listening for RTMP publisher on " << config_.input_url;
    int rc = avformat_open_input(&input_ctx_, config_.input_url.c_str(), nullptr, &options);
    av_dict_free(&options);
    if (rc < 0) {
        LOG << "avformat_open_input(" << config_.input_url << ") failed: " << av_error_string(rc);
        return false;
    }

    rc = avformat_find_stream_info(input_ctx_, nullptr);
    if (rc < 0) {
        LOG << "avformat_find_stream_info() failed: " << av_error_string(rc);
        return false;
    }

    for (unsigned int i = 0; i < input_ctx_->nb_streams; ++i) {
        const AVStream* stream = input_ctx_->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video_stream_index_ < 0) {
            video_stream_index_ = static_cast<int>(i);
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_stream_index_ < 0) {
            audio_stream_index_ = static_cast<int>(i);
        }
    }

    if (video_stream_index_ < 0 && audio_stream_index_ < 0) {
        LOG << "Input does not contain any audio or video streams";
        return false;
    }

    LOG << "Accepted RTMP publisher on " << config_.input_url << " with " << input_ctx_->nb_streams << " stream(s)";
    return true;
}

bool RtmpRelay::open_output() {
    int rc = avformat_alloc_output_context2(&output_ctx_, nullptr, "flv", config_.output_url.c_str());
    if (rc < 0 || output_ctx_ == nullptr) {
        LOG << "avformat_alloc_output_context2(" << config_.output_url << ") failed: " << av_error_string(rc);
        return false;
    }

    output_ctx_->interrupt_callback = {stop_aware_interrupt, this};

    for (unsigned int i = 0; i < input_ctx_->nb_streams; ++i) {
        const AVStream* input_stream = input_ctx_->streams[i];
        AVStream* output_stream = avformat_new_stream(output_ctx_, nullptr);
        if (output_stream == nullptr) {
            LOG << "avformat_new_stream() failed for stream " << i;
            close_output();
            return false;
        }

        rc = avcodec_parameters_copy(output_stream->codecpar, input_stream->codecpar);
        if (rc < 0) {
            LOG << "avcodec_parameters_copy() failed for stream " << i << ": " << av_error_string(rc);
            close_output();
            return false;
        }

        output_stream->codecpar->codec_tag = 0;
        output_stream->time_base = input_stream->time_base;
    }

    if ((output_ctx_->oformat->flags & AVFMT_NOFILE) == 0) {
        AVDictionary* options = nullptr;
        av_dict_set(&options, "tcp_nodelay", "1", 0);
        rc = avio_open2(&output_ctx_->pb, config_.output_url.c_str(), AVIO_FLAG_WRITE, &output_ctx_->interrupt_callback, &options);
        av_dict_free(&options);
        if (rc < 0) {
            LOG << "avio_open2(" << config_.output_url << ") failed: " << av_error_string(rc);
            close_output();
            return false;
        }
    }

    rc = avformat_write_header(output_ctx_, nullptr);
    if (rc < 0) {
        LOG << "avformat_write_header() failed: " << av_error_string(rc);
        close_output();
        return false;
    }

    waiting_for_video_keyframe_ = true;
    next_output_retry_at_ = std::chrono::steady_clock::time_point::min();
    LOG << "Opened output " << config_.output_url;
    return true;
}

bool RtmpRelay::ensure_output() {
    if (output_ctx_ != nullptr)
        return true;

    const auto now = std::chrono::steady_clock::now();
    if (now < next_output_retry_at_)
        return false;

    if (open_output())
        return true;

    next_output_retry_at_ = now + kOutputRetryDelay;
    return false;
}

void RtmpRelay::handle_output_disconnect(const std::string& error_text) {
    LOG << "Output connection lost: " << error_text << ". Waiting for listener to return";
    close_output();
    waiting_for_video_keyframe_ = true;
    next_output_retry_at_ = std::chrono::steady_clock::now() + kOutputRetryDelay;
}

void RtmpRelay::close_output() {
    if (output_ctx_ == nullptr)
        return;

    if ((output_ctx_->oformat->flags & AVFMT_NOFILE) == 0 && output_ctx_->pb != nullptr)
        avio_closep(&output_ctx_->pb);

    avformat_free_context(output_ctx_);
    output_ctx_ = nullptr;
}

void RtmpRelay::close_input() {
    if (input_ctx_ == nullptr)
        return;

    avformat_close_input(&input_ctx_);
    input_ctx_ = nullptr;
}

void RtmpRelay::reset_input_state() {
    video_stream_index_ = -1;
    audio_stream_index_ = -1;
    video_frame_count_.store(0, std::memory_order_relaxed);
    video_key_count_.store(0, std::memory_order_relaxed);
    video_bytes_.store(0, std::memory_order_relaxed);
    audio_frame_count_.store(0, std::memory_order_relaxed);
    audio_bytes_.store(0, std::memory_order_relaxed);
    last_video_pts_.store(AV_NOPTS_VALUE, std::memory_order_relaxed);
    last_audio_pts_.store(AV_NOPTS_VALUE, std::memory_order_relaxed);
    prev_video_frames_ = 0;
    prev_video_keys_ = 0;
    prev_video_bytes_ = 0;
    prev_audio_frames_ = 0;
    prev_audio_bytes_ = 0;
    waiting_for_video_keyframe_ = true;
    need_video_dts_offset_ = (last_written_video_dts_ != AV_NOPTS_VALUE);
    need_audio_dts_offset_ = (last_written_audio_dts_ != AV_NOPTS_VALUE);
    video_dts_offset_ = 0;
    audio_dts_offset_ = 0;
}

void RtmpRelay::log_stream_map() const {
    for (unsigned int i = 0; i < input_ctx_->nb_streams; ++i) {
        const AVStream* stream = input_ctx_->streams[i];
        const char* media_type = av_get_media_type_string(stream->codecpar->codec_type);
        const AVCodecDescriptor* codec = avcodec_descriptor_get(stream->codecpar->codec_id);
        LOG << "stream #" << i << " type=" << (media_type == nullptr ? "unknown" : media_type) << " codec=" << (codec == nullptr ? "unknown" : codec->name)
            << " time_base=" << stream->time_base.num << '/' << stream->time_base.den;
    }
}

void RtmpRelay::start_stats_timer(std::function<void()> on_tick) {
    stats_timer_.expires_after(stats_period_);
    stats_timer_.async_wait([this, on_tick = std::move(on_tick)](const asio::error_code& ec) {
        if (ec == asio::error::operation_aborted)
            return;
        on_tick();
        start_stats_timer(std::move(on_tick));
    });
}

void RtmpRelay::cancel_stats_timer() {
    stats_timer_.cancel();
}

void RtmpRelay::print_stats() {
    if (input_ctx_ == nullptr)
        return;

    const bool have_video = video_stream_index_ >= 0;
    const bool have_audio = audio_stream_index_ >= 0;
    if (!have_video && !have_audio)
        return;

    const double period = static_cast<double>(stats_period_.count());

    // --- PTS line ---
    const int64_t v_pts = last_video_pts_.load(std::memory_order_relaxed);
    const int64_t a_pts = last_audio_pts_.load(std::memory_order_relaxed);

    std::ostringstream pts_line;
    pts_line << "[pts]";
    double v_sec = 0, a_sec = 0;
    if (have_audio && a_pts != AV_NOPTS_VALUE) {
        a_sec = pts_to_seconds(a_pts, input_ctx_->streams[audio_stream_index_]->time_base);
        pts_line << " audio: " << format_hms(a_sec);
    }
    if (have_video && v_pts != AV_NOPTS_VALUE) {
        v_sec = pts_to_seconds(v_pts, input_ctx_->streams[video_stream_index_]->time_base);
        pts_line << (have_audio && a_pts != AV_NOPTS_VALUE ? ", " : " ") << "video: " << format_hms(v_sec);
    }
    if (have_audio && a_pts != AV_NOPTS_VALUE && have_video && v_pts != AV_NOPTS_VALUE) {
        const double diff = a_sec - v_sec;
        pts_line << ", diff: " << (diff >= 0 ? "+" : "") << std::fixed << std::setprecision(3) << diff;
    }
    LOG << pts_line.str();

    // --- video ---
    if (have_video) {
        const uint64_t vf = video_frame_count_.load(std::memory_order_relaxed);
        const uint64_t vk = video_key_count_.load(std::memory_order_relaxed);
        const uint64_t vb = video_bytes_.load(std::memory_order_relaxed);

        LOG << "[v:" << stats_period_.count() << "s] frames: " << (vf - prev_video_frames_) << " (" << (vk - prev_video_keys_) << " key)"
            << ", bitrate: " << bitrate_kbps(vb - prev_video_bytes_, period) << " kbps";

        if (v_pts != AV_NOPTS_VALUE && v_sec > 0)
            LOG << "[v:all] frames: " << vf << " (" << vk << " key), bitrate: " << bitrate_kbps(vb, v_sec) << " kbps";

        prev_video_frames_ = vf;
        prev_video_keys_ = vk;
        prev_video_bytes_ = vb;
    }

    // --- audio ---
    if (have_audio) {
        const uint64_t af = audio_frame_count_.load(std::memory_order_relaxed);
        const uint64_t ab = audio_bytes_.load(std::memory_order_relaxed);

        LOG << "[a:" << stats_period_.count() << "s] frames: " << (af - prev_audio_frames_)
            << ", bitrate: " << bitrate_kbps(ab - prev_audio_bytes_, period) << " kbps";

        if (a_pts != AV_NOPTS_VALUE && a_sec > 0)
            LOG << "[a:all] frames: " << af << ", bitrate: " << bitrate_kbps(ab, a_sec) << " kbps";

        prev_audio_frames_ = af;
        prev_audio_bytes_ = ab;
    }
}

int RelayApp::run() {
    install_sigpipe_handler();

    relay_thread_ = std::thread([this] {
        exit_code_ = relay_.run();
        asio::post(io_context_, [this] { io_context_.stop(); });
    });

    signals_.async_wait([this](asio::error_code ec, int signal_number) { handle_stop_signal(ec, signal_number); });

    io_context_.run();
    relay_.request_stop();

    relay_.cancel_stats_timer();

    if (relay_thread_.joinable())
        relay_thread_.join();

    return exit_code_.value_or(1);
}

void RelayApp::print_stats() {
    relay_.print_stats();
}

void RelayApp::handle_stop_signal(asio::error_code ec, int signal_number) {
    if (ec == asio::error::operation_aborted)
        return;

    if (ec) {
        LOG << "signal handling failed: " << ec.message();
        return;
    }

    LOG << "Received signal " << signal_number << ", stopping relay";
    relay_.request_stop();
    signals_.cancel();
    io_context_.stop();
}

void RelayApp::install_sigpipe_handler() {
    struct sigaction ignore_sa{};
    ignore_sa.sa_handler = SIG_IGN;
    sigemptyset(&ignore_sa.sa_mask);
    ignore_sa.sa_flags = 0;
    if (sigaction(SIGPIPE, &ignore_sa, nullptr) < 0)
        LOG << "sigaction(SIGPIPE) failed: " << std::strerror(errno);
}
