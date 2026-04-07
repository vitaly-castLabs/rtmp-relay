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

std::string format_ts(int64_t ts, AVRational time_base) {
    if (ts == AV_NOPTS_VALUE)
        return "nop";

    std::ostringstream oss;
    oss << ts << " (" << std::fixed << std::setprecision(3) << (static_cast<double>(ts) * av_q2d(time_base)) << "s)";
    return oss.str();
}

int stop_aware_interrupt(void* opaque) {
    const auto* relay = static_cast<const RtmpRelay*>(opaque);
    return relay != nullptr && relay->stop_requested() ? 1 : 0;
}

}  // namespace

RtmpRelay::RtmpRelay(RelayConfig config) : config_(std::move(config)) {}

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
    if (!open_input())
        return 1;

    LOG << "Relay started: " << config_.input_url << " -> " << config_.output_url;
    log_stream_map();

    if (!open_output())
        LOG << "Output is not available yet; relay will keep ingesting and retry";

    AVPacket packet {};

    while (!stop_requested_.load(std::memory_order_relaxed)) {
        const int read_rc = av_read_frame(input_ctx_, &packet);
        if (read_rc == AVERROR_EOF) {
            LOG << "Input stream ended";
            if (output_ctx_ != nullptr)
                av_write_trailer(output_ctx_);
            return 0;
        }
        if (read_rc == AVERROR(EIO) && video_frame_count_ > 0) {
            LOG << "Input stream closed by peer";
            if (output_ctx_ != nullptr)
                av_write_trailer(output_ctx_);
            return 0;
        }
        if (read_rc < 0) {
            if (stop_requested_.load(std::memory_order_relaxed) && read_rc == AVERROR_EXIT) {
                LOG << "Relay stopped while waiting for input";
                if (output_ctx_ != nullptr)
                    av_write_trailer(output_ctx_);
                return 0;
            }

            LOG << "av_read_frame() failed: " << av_error_string(read_rc);
            return 1;
        }

        AVStream* input_stream = input_ctx_->streams[packet.stream_index];

        if (packet.stream_index == video_stream_index_) {
            ++video_frame_count_;
            LOG << "video frame #" << video_frame_count_
                << " size=" << packet.size
                << " pts=" << format_ts(packet.pts, input_stream->time_base)
                << " dts=" << format_ts(packet.dts, input_stream->time_base)
                << " duration=" << format_ts(packet.duration, input_stream->time_base)
                << " key=" << ((packet.flags & AV_PKT_FLAG_KEY) != 0 ? "yes" : "no");
        }

        if (!ensure_output()) {
            av_packet_unref(&packet);
            continue;
        }

        if (waiting_for_video_keyframe_) {
            const bool is_video_packet = packet.stream_index == video_stream_index_;
            const bool is_keyframe = (packet.flags & AV_PKT_FLAG_KEY) != 0;
            if (!is_video_packet || !is_keyframe) {
                av_packet_unref(&packet);
                continue;
            }

            waiting_for_video_keyframe_ = false;
            LOG << "Resumed output on video keyframe #" << video_frame_count_;
        }

        AVStream* output_stream = output_ctx_->streams[packet.stream_index];
        av_packet_rescale_ts(&packet, input_stream->time_base, output_stream->time_base);
        packet.pos = -1;

        const int write_rc = av_interleaved_write_frame(output_ctx_, &packet);
        av_packet_unref(&packet);
        if (write_rc < 0) {
            if (stop_requested_.load(std::memory_order_relaxed) && write_rc == AVERROR_EXIT) {
                LOG << "Relay stopped while writing output";
                if (output_ctx_ != nullptr)
                    av_write_trailer(output_ctx_);
                return 0;
            }

            handle_output_disconnect(av_error_string(write_rc));
        }
    }

    LOG << "Stop requested";
    if (output_ctx_ != nullptr)
        av_write_trailer(output_ctx_);
    return 0;
}

bool RtmpRelay::open_input() {
    input_ctx_ = avformat_alloc_context();
    if (input_ctx_ == nullptr) {
        LOG << "avformat_alloc_context() failed for input";
        return false;
    }

    input_ctx_->interrupt_callback = {stop_aware_interrupt, this};

    AVDictionary* options = nullptr;
    if (config_.listen_input) {
        av_dict_set(&options, "listen", "1", 0);
        LOG << "Listening for RTMP publisher on " << config_.input_url;
    }

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
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_index_ = static_cast<int>(i);
            break;
        }
    }

    if (video_stream_index_ < 0) {
        LOG << "Input does not contain a video stream";
        return false;
    }

    if (config_.listen_input) {
        LOG << "Accepted RTMP publisher on " << config_.input_url << " with " << input_ctx_->nb_streams << " stream(s)";
    } else {
        LOG << "Opened input " << config_.input_url << " with " << input_ctx_->nb_streams << " stream(s)";
    }
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

void RtmpRelay::log_stream_map() const {
    for (unsigned int i = 0; i < input_ctx_->nb_streams; ++i) {
        const AVStream* stream = input_ctx_->streams[i];
        const char* media_type = av_get_media_type_string(stream->codecpar->codec_type);
        const AVCodecDescriptor* codec = avcodec_descriptor_get(stream->codecpar->codec_id);
        LOG << "stream #" << i
            << " type=" << (media_type == nullptr ? "unknown" : media_type)
            << " codec=" << (codec == nullptr ? "unknown" : codec->name)
            << " time_base=" << stream->time_base.num << '/' << stream->time_base.den;
    }
}

RelayApp::RelayApp(RelayConfig config) : signals_(io_context_, SIGINT, SIGTERM), relay_(std::move(config)) {}

int RelayApp::run() {
    install_sigpipe_handler();

    relay_thread_ = std::thread([this] {
        exit_code_ = relay_.run();
        asio::post(io_context_, [this] { io_context_.stop(); });
    });

    signals_.async_wait([this](asio::error_code ec, int signal_number) {
        handle_stop_signal(ec, signal_number);
    });

    io_context_.run();
    relay_.request_stop();

    if (relay_thread_.joinable())
        relay_thread_.join();

    return exit_code_.value_or(1);
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
    struct sigaction ignore_sa {};
    ignore_sa.sa_handler = SIG_IGN;
    sigemptyset(&ignore_sa.sa_mask);
    ignore_sa.sa_flags = 0;
    if (sigaction(SIGPIPE, &ignore_sa, nullptr) < 0)
        LOG << "sigaction(SIGPIPE) failed: " << std::strerror(errno);
}
