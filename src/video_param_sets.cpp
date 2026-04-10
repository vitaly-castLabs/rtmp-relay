#include "video_param_sets.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <cstdint>
#include <vector>

namespace {

constexpr uint8_t kH264NalTypeIdr = 5;
constexpr uint8_t kH264NalTypeSps = 7;
constexpr uint8_t kH264NalTypePps = 8;

constexpr uint8_t kHevcNalTypeVps = 32;
constexpr uint8_t kHevcNalTypeSps = 33;
constexpr uint8_t kHevcNalTypePps = 34;
constexpr uint8_t kHevcNalTypeBlaWLp = 16;
constexpr uint8_t kHevcNalTypeRsvIrapVcl23 = 23;

uint32_t read_avcc_length(const uint8_t* data, int length_size) {
    uint32_t value = 0;
    for (int i = 0; i < length_size; ++i)
        value = (value << 8) | data[i];
    return value;
}

void append_avcc_length(std::vector<uint8_t>& dst, int length_size, size_t nalu_size) {
    const size_t base = dst.size();
    dst.resize(base + length_size);
    for (int i = length_size - 1; i >= 0; --i) {
        dst[base + i] = static_cast<uint8_t>(nalu_size & 0xFF);
        nalu_size >>= 8;
    }
}

uint8_t get_nal_unit_type(enum AVCodecID codec_id, const uint8_t* nal) {
    if (codec_id == AV_CODEC_ID_H264)
        return nal[0] & 0x1F;
    if (codec_id == AV_CODEC_ID_HEVC)
        return (nal[0] >> 1) & 0x3F;
    return 0;
}

bool is_irap(enum AVCodecID codec_id, uint8_t nal_type) {
    if (codec_id == AV_CODEC_ID_H264)
        return nal_type == kH264NalTypeIdr;
    if (codec_id == AV_CODEC_ID_HEVC)
        return nal_type >= kHevcNalTypeBlaWLp && nal_type <= kHevcNalTypeRsvIrapVcl23;
    return false;
}

bool is_parameter_set(enum AVCodecID codec_id, uint8_t nal_type) {
    if (codec_id == AV_CODEC_ID_H264)
        return nal_type == kH264NalTypeSps || nal_type == kH264NalTypePps;
    if (codec_id == AV_CODEC_ID_HEVC)
        return nal_type == kHevcNalTypeVps || nal_type == kHevcNalTypeSps || nal_type == kHevcNalTypePps;
    return false;
}

bool parse_length_prefixed_packet(enum AVCodecID codec_id, const uint8_t* data, size_t size, int length_size, bool& has_irap, bool& has_all_param_sets) {
    bool has_vps = codec_id != AV_CODEC_ID_HEVC;
    bool has_sps = false;
    bool has_pps = false;
    has_irap = false;

    size_t offset = 0;
    while (offset + static_cast<size_t>(length_size) <= size) {
        const uint32_t nalu_size = read_avcc_length(data + offset, length_size);
        offset += static_cast<size_t>(length_size);
        if (nalu_size == 0 || nalu_size > size - offset)
            return false;

        const uint8_t nal_type = get_nal_unit_type(codec_id, data + offset);
        if (is_irap(codec_id, nal_type))
            has_irap = true;
        if (codec_id == AV_CODEC_ID_HEVC && nal_type == kHevcNalTypeVps)
            has_vps = true;
        if (nal_type == kH264NalTypeSps || nal_type == kHevcNalTypeSps)
            has_sps = true;
        if (nal_type == kH264NalTypePps || nal_type == kHevcNalTypePps)
            has_pps = true;

        offset += nalu_size;
    }

    has_all_param_sets = has_vps && has_sps && has_pps;
    return offset == size;
}

bool build_h264_avcc_param_sets(const AVCodecParameters* codecpar, std::vector<uint8_t>& out, int& length_size) {
    out.clear();

    const uint8_t* extradata = codecpar->extradata;
    const size_t extradata_size = static_cast<size_t>(codecpar->extradata_size);
    if (!extradata || extradata_size < 7 || extradata[0] != 1)
        return false;

    length_size = (extradata[4] & 0x03) + 1;
    if (length_size < 1 || length_size > 4)
        return false;

    size_t offset = 5;
    const size_t num_sps = extradata[offset++] & 0x1F;
    for (size_t i = 0; i < num_sps; ++i) {
        if (offset + 2 > extradata_size)
            return false;
        const size_t nalu_size = (static_cast<size_t>(extradata[offset]) << 8) | static_cast<size_t>(extradata[offset + 1]);
        offset += 2;
        if (offset + nalu_size > extradata_size)
            return false;
        append_avcc_length(out, length_size, nalu_size);
        out.insert(out.end(), extradata + offset, extradata + offset + nalu_size);
        offset += nalu_size;
    }

    if (offset + 1 > extradata_size)
        return false;

    const size_t num_pps = extradata[offset++];
    for (size_t i = 0; i < num_pps; ++i) {
        if (offset + 2 > extradata_size)
            return false;
        const size_t nalu_size = (static_cast<size_t>(extradata[offset]) << 8) | static_cast<size_t>(extradata[offset + 1]);
        offset += 2;
        if (offset + nalu_size > extradata_size)
            return false;
        append_avcc_length(out, length_size, nalu_size);
        out.insert(out.end(), extradata + offset, extradata + offset + nalu_size);
        offset += nalu_size;
    }

    return !out.empty();
}

bool build_hevc_hvcc_param_sets(const AVCodecParameters* codecpar, std::vector<uint8_t>& out, int& length_size) {
    out.clear();

    const uint8_t* extradata = codecpar->extradata;
    const size_t extradata_size = static_cast<size_t>(codecpar->extradata_size);
    if (!extradata || extradata_size < 23 || extradata[0] != 1)
        return false;

    size_t offset = 21;
    length_size = (extradata[offset++] & 0x03) + 1;
    if (length_size < 1 || length_size > 4 || offset >= extradata_size)
        return false;

    const size_t num_arrays = extradata[offset++];
    for (size_t i = 0; i < num_arrays; ++i) {
        if (offset + 3 > extradata_size)
            return false;

        const uint8_t nal_type = extradata[offset++] & 0x3F;
        const size_t count = (static_cast<size_t>(extradata[offset]) << 8) | static_cast<size_t>(extradata[offset + 1]);
        offset += 2;

        for (size_t j = 0; j < count; ++j) {
            if (offset + 2 > extradata_size)
                return false;
            const size_t nalu_size = (static_cast<size_t>(extradata[offset]) << 8) | static_cast<size_t>(extradata[offset + 1]);
            offset += 2;
            if (offset + nalu_size > extradata_size)
                return false;
            if (is_parameter_set(AV_CODEC_ID_HEVC, nal_type)) {
                append_avcc_length(out, length_size, nalu_size);
                out.insert(out.end(), extradata + offset, extradata + offset + nalu_size);
            }
            offset += nalu_size;
        }
    }

    return !out.empty();
}

bool build_param_sets_for_stream(const AVCodecParameters* codecpar, std::vector<uint8_t>& out, int& length_size) {
    if (codecpar->codec_id == AV_CODEC_ID_H264)
        return build_h264_avcc_param_sets(codecpar, out, length_size);
    if (codecpar->codec_id == AV_CODEC_ID_HEVC)
        return build_hevc_hvcc_param_sets(codecpar, out, length_size);
    return false;
}

} // namespace

bool prepend_param_sets_for_transform(const AVCodecParameters* codecpar, const AVPacket& packet, std::vector<uint8_t>& out) {
    if ((codecpar->codec_id != AV_CODEC_ID_H264 && codecpar->codec_id != AV_CODEC_ID_HEVC) || (packet.flags & AV_PKT_FLAG_KEY) == 0 || !packet.data ||
        packet.size <= 0)
        return false;

    int length_size = 0;
    std::vector<uint8_t> param_sets;
    if (!build_param_sets_for_stream(codecpar, param_sets, length_size))
        return false;

    bool has_irap = false;
    bool has_all_param_sets = false;
    if (!parse_length_prefixed_packet(codecpar->codec_id, packet.data, static_cast<size_t>(packet.size), length_size, has_irap, has_all_param_sets))
        return false;
    if (!has_irap || has_all_param_sets)
        return false;

    out = param_sets;
    out.insert(out.end(), packet.data, packet.data + packet.size);
    return true;
}
