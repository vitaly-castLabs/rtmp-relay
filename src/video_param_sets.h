#pragma once

extern "C" {
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
}

#include <vector>

bool prepend_param_sets_for_transform(const AVCodecParameters* codecpar, const AVPacket& packet, std::vector<uint8_t>& out);
