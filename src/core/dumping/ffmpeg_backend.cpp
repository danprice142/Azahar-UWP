// Copyright 2018 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <span>
#include <unordered_map>
#include "common/assert.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/param_package.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "core/core_timing.h"
#include "core/dumping/ffmpeg_backend.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"

using namespace DynamicLibrary;

namespace VideoDumper {

void InitializeFFmpegLibraries() {
    static bool initialized = false;
    if (initialized) {
        return;
    }

    FFmpeg::avformat_network_init();
    initialized = true;
}

AVDictionary* ToAVDictionary(const std::string& serialized) {
    Common::ParamPackage param_package{serialized};
    AVDictionary* result = nullptr;
    for (const auto& [key, value] : param_package) {
        FFmpeg::av_dict_set(&result, key.c_str(), value.c_str(), 0);
    }
    return result;
}

FFmpegStream::~FFmpegStream() {
    Free();
}

bool FFmpegStream::Init(FFmpegMuxer& muxer) {
    InitializeFFmpegLibraries();

    format_context = muxer.format_context.get();
    format_context_mutex = &muxer.format_context_mutex;

    return true;
}

void FFmpegStream::Free() {
    codec_context.reset();
}

void FFmpegStream::Flush() {
    SendFrame(nullptr);
}

void FFmpegStream::WritePacket(AVPacket* packet) {
    FFmpeg::av_packet_rescale_ts(packet, codec_context->time_base, stream->time_base);
    packet->stream_index = stream->index;
    {
        std::scoped_lock lock{*format_context_mutex};
        FFmpeg::av_interleaved_write_frame(format_context, packet);
    }
}

bool FFmpegStream::SendFrame(AVFrame* frame) {
    // Initialize packet
    AVPacket* packet = FFmpeg::av_packet_alloc();
    if (!packet) {
        LOG_ERROR(Render, "Frame dropped: av_packet_alloc failed");
        return false;
    }
    SCOPE_EXIT({ FFmpeg::av_packet_free(&packet); });

    packet->data = nullptr;
    packet->size = 0;

    const auto receive_packets = [&] {
        int error = 1;
        while (error >= 0) {
            error = FFmpeg::avcodec_receive_packet(codec_context.get(), packet);
            if (error == AVERROR(EAGAIN) || error == AVERROR_EOF)
                return;
            if (error < 0) {
                LOG_ERROR(Render, "Frame dropped: could not encode audio");
                return;
            } else {
                // Write frame to video file
                WritePacket(packet);
            }
        }
    };

    // Encode frame
    int sent = FFmpeg::avcodec_send_frame(codec_context.get(), frame);
    if (sent == AVERROR(EAGAIN)) {
        // An asynchronous encoder still holds the previous frame; drain it and offer this one again.
        receive_packets();
        sent = FFmpeg::avcodec_send_frame(codec_context.get(), frame);
    }
    if (sent < 0) {
        if (sent != AVERROR(EAGAIN)) {
            LOG_ERROR(Render, "Frame dropped: could not send frame");
        }
        return false;
    }
    receive_packets();
    return true;
}

FFmpegVideoStream::~FFmpegVideoStream() {
    Free();
}

// This is modified from libavcodec/decode.c
// The original version was broken
static AVPixelFormat GetPixelFormat(AVCodecContext* avctx, const AVPixelFormat* fmt) {
    // Choose a software pixel format if any, prefering those in the front of the list
    for (int i = 0; fmt[i] != AV_PIX_FMT_NONE; i++) {
        const AVPixFmtDescriptor* desc = FFmpeg::av_pix_fmt_desc_get(fmt[i]);
        if (!(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
            return fmt[i];
        }
    }

    // Finally, traverse the list in order and choose the first entry
    // with no external dependencies (if there is no hardware configuration
    // information available then this just picks the first entry).
    for (int i = 0; fmt[i] != AV_PIX_FMT_NONE; i++) {
        const AVCodecHWConfig* config;
        for (int j = 0;; j++) {
            config = FFmpeg::avcodec_get_hw_config(avctx->codec, j);
            if (!config || config->pix_fmt == fmt[i]) {
                break;
            }
        }
        if (!config) {
            // No specific config available, so the decoder must be able
            // to handle this format without any additional setup.
            return fmt[i];
        }
        if (config->methods & AV_CODEC_HW_CONFIG_METHOD_INTERNAL) {
            // Usable with only internal setup.
            return fmt[i];
        }
    }

    // Nothing is usable, give up.
    return AV_PIX_FMT_NONE;
}

bool FFmpegVideoStream::Init(FFmpegMuxer& muxer, const Layout::FramebufferLayout& layout_,
                             u32 target_fps_, bool low_latency_) {
    InitializeFFmpegLibraries();

    if (!FFmpegStream::Init(muxer)) {
        return false;
    }

    layout = layout_;
    target_fps = std::clamp(target_fps_, 1U, 60U);
    low_latency = low_latency_;
    frame_count = 0;
    first_frame_pending = true;
    next_keyframe_pts = 0;

    // Initialize video codec
    codec = FFmpeg::avcodec_find_encoder_by_name(Settings::values.video_encoder.c_str());
    if (!codec) {
        LOG_ERROR(Render, "Could not find video encoder {}", Settings::values.video_encoder);
        return false;
    }
    const std::string_view encoder_name{Settings::values.video_encoder};
    mf_streaming = low_latency && (encoder_name == "h264_mf" || encoder_name == "hevc_mf");

    // Media Foundation defaults to its software transform; a stream tries the hardware one first.
    if (!OpenCodec(mf_streaming) && !(mf_streaming && OpenCodec(false))) {
        return false;
    }

    // Create video stream
    stream = FFmpeg::avformat_new_stream(format_context, codec);
    if (!stream ||
        FFmpeg::avcodec_parameters_from_context(stream->codecpar, codec_context.get()) < 0) {
        LOG_ERROR(Render, "Could not create video stream");
        return false;
    }

    stream->time_base = codec_context->time_base;

    // Allocate frames
    current_frame.reset(FFmpeg::av_frame_alloc());
    filtered_frame.reset(FFmpeg::av_frame_alloc());

    if (requires_hw_frames) {
        hw_frame.reset(FFmpeg::av_frame_alloc());
        if (FFmpeg::av_hwframe_get_buffer(codec_context->hw_frames_ctx, hw_frame.get(), 0) < 0) {
            LOG_ERROR(Render, "Could not allocate buffer for HW frame");
            return false;
        }
    }

    return InitFilters();
}

void FFmpegVideoStream::Free() {
    FFmpegStream::Free();

    current_frame.reset();
    filtered_frame.reset();
    hw_frame.reset();
    filter_graph.reset();
    source_context = nullptr;
    sink_context = nullptr;
}

bool FFmpegVideoStream::OpenCodec(bool hardware) {
    codec_context.reset(FFmpeg::avcodec_alloc_context3(codec));
    if (!codec_context) {
        LOG_ERROR(Render, "Could not allocate video codec context");
        return false;
    }

    // Configure video codec context
    codec_context->codec_type = AVMEDIA_TYPE_VIDEO;
    codec_context->bit_rate = Settings::values.video_bitrate;
    codec_context->width = layout.width;
    codec_context->height = layout.height;
    // The filter below resamples the emulated frame cadence to this stream's requested rate.
    codec_context->time_base.num = 1;
    codec_context->time_base.den = static_cast<int>(target_fps);
    codec_context->gop_size = low_latency ? std::max(1, static_cast<int>(target_fps / 2)) : 12;
    if (low_latency) {
        codec_context->max_b_frames = 0;
        codec_context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    }
    if (mf_streaming) {
        codec_context->framerate = AVRational{static_cast<int>(target_fps), 1};
        if (codec->id == AV_CODEC_ID_H264) {
            codec_context->profile = FF_PROFILE_H264_BASELINE;
        }
    }

    // Get pixel format for codec
    auto options = ToAVDictionary(Settings::values.video_encoder_options);
    if (low_latency) {
        const std::string_view encoder_name{Settings::values.video_encoder};
        if (encoder_name == "h264_mf" || encoder_name == "hevc_mf") {
            // Media Foundation exposes rate-control hints as private encoder options; user options
            // take precedence.
            FFmpeg::av_dict_set(&options, "rate_control", "cbr", AV_DICT_DONT_OVERWRITE);
            FFmpeg::av_dict_set(&options, "scenario", "live_streaming", AV_DICT_DONT_OVERWRITE);
            FFmpeg::av_dict_set(&options, "hw_encoding", hardware ? "1" : "0",
                                AV_DICT_DONT_OVERWRITE);
        } else if (encoder_name == "libx264") {
            FFmpeg::av_dict_set(&options, "tune", "zerolatency", AV_DICT_DONT_OVERWRITE);
        }
    }
    const AVDictionaryEntry* hw_option = FFmpeg::av_dict_get(options, "hw_encoding", nullptr, 0);
    hardware_encoding = hw_option && (std::string_view{hw_option->value} == "1" ||
                                      std::string_view{hw_option->value} == "true");
    auto pixel_format_opt = FFmpeg::av_dict_get(options, "pixel_format", nullptr, 0);
    if (pixel_format_opt) {
        sw_pixel_format = FFmpeg::av_get_pix_fmt(pixel_format_opt->value);
    } else if (mf_streaming) {
        // The hardware transform takes NV12; the software one accepts it as well.
        sw_pixel_format = AV_PIX_FMT_NV12;
    } else if (codec->pix_fmts) {
        sw_pixel_format = GetPixelFormat(codec_context.get(), codec->pix_fmts);
    } else {
        sw_pixel_format = AV_PIX_FMT_YUV420P;
    }
    if (sw_pixel_format == AV_PIX_FMT_NONE) {
        // This encoder requires HW context configuration.
        if (!InitHWContext(codec)) {
            LOG_ERROR(Render, "Failed to initialize HW context");
            return false;
        }
    } else {
        requires_hw_frames = false;
        codec_context->pix_fmt = sw_pixel_format;
    }

    if (format_context->oformat->flags & AVFMT_GLOBALHEADER) {
        codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    if (FFmpeg::avcodec_open2(codec_context.get(), codec, &options) < 0) {
        LOG_ERROR(Render, "Could not open {} video codec",
                  hardware_encoding ? "hardware" : "software");
        return false;
    }

    if (FFmpeg::av_dict_count(options) != 0) { // Successfully set options are removed from the dict
        char* buf = nullptr;
        FFmpeg::av_dict_get_string(options, &buf, ':', ';');
        LOG_WARNING(Render, "Video encoder options not found: {}", buf);
    }
    if (mf_streaming) {
        LOG_INFO(Render, "Streaming encoder: {} {} {}x{} {} fps {} kbps",
                 Settings::values.video_encoder, hardware_encoding ? "hardware" : "software",
                 layout.width, layout.height, target_fps, Settings::values.video_bitrate / 1000);
    }
    return true;
}

void FFmpegVideoStream::ProcessFrame(VideoFrame& frame) {
    if (frame.width != layout.width || frame.height != layout.height) {
        LOG_ERROR(Render, "Frame dropped: resolution does not match");
        return;
    }
    // Prepare frame
    current_frame->data[0] = frame.data.data();
    current_frame->linesize[0] = frame.stride;
    current_frame->format = pixel_format;
    current_frame->width = layout.width;
    current_frame->height = layout.height;
    current_frame->pts = frame_count++;

    // Filter the frame
    if (FFmpeg::av_buffersrc_add_frame(source_context, current_frame.get()) < 0) {
        LOG_ERROR(Render, "Video frame dropped: Could not add frame to filter graph");
        return;
    }
    while (true) {
        const int error = FFmpeg::av_buffersink_get_frame(sink_context, filtered_frame.get());
        if (error == AVERROR(EAGAIN) || error == AVERROR_EOF) {
            return;
        }
        if (error < 0) {
            LOG_ERROR(Render, "Video frame dropped: Could not receive frame from filter graph");
            return;
        } else {
            if (requires_hw_frames) {
                if (FFmpeg::av_hwframe_transfer_data(hw_frame.get(), filtered_frame.get(), 0) < 0) {
                    LOG_ERROR(Render, "Video frame dropped: Could not upload to HW frame");
                    return;
                }
                SendFrame(hw_frame.get());
            } else {
                if (mf_streaming) {
                    // MF ignores gop_size and needs a nonzero sample duration; periodic I-frames
                    // let a UDP receiver recover or join midstream.
                    filtered_frame->duration = 1;
                    filtered_frame->pict_type = filtered_frame->pts >= next_keyframe_pts
                                                    ? AV_PICTURE_TYPE_I
                                                    : AV_PICTURE_TYPE_NONE;
                }
                bool sent = SendFrame(filtered_frame.get());
                if (!sent && first_frame_pending && hardware_encoding) {
                    LOG_WARNING(Render, "Hardware encoder rejected its first frame, using software");
                    if (OpenCodec(false) && FFmpeg::avcodec_parameters_from_context(
                                                stream->codecpar, codec_context.get()) >= 0) {
                        sent = SendFrame(filtered_frame.get());
                    }
                }
                if (sent) {
                    first_frame_pending = false;
                    if (mf_streaming && filtered_frame->pict_type == AV_PICTURE_TYPE_I) {
                        next_keyframe_pts = filtered_frame->pts + static_cast<s64>(target_fps);
                    }
                }
            }

            FFmpeg::av_frame_unref(filtered_frame.get());
        }
    }
}

bool FFmpegVideoStream::InitHWContext(const AVCodec* codec) {
    for (std::size_t i = 0; codec->pix_fmts[i] != AV_PIX_FMT_NONE; ++i) {
        const AVCodecHWConfig* config;
        for (int j = 0;; ++j) {
            config = FFmpeg::avcodec_get_hw_config(codec, j);
            if (!config || config->pix_fmt == codec->pix_fmts[i]) {
                break;
            }
        }
        // If we are at this point, there should not be any possible HW format that does not
        // need configuration.
        ASSERT_MSG(config, "HW pixel format that does not need config should have been selected");

        if (!(config->methods & (AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX |
                                 AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX))) {
            // Maybe this format requires ad-hoc configuration, unsupported.
            continue;
        }

        codec_context->pix_fmt = codec->pix_fmts[i];

        // Create HW device context
        AVBufferRef* hw_device_context;
        SCOPE_EXIT({ FFmpeg::av_buffer_unref(&hw_device_context); });

        // TODO: Provide the argument here somehow.
        // This is necessary for some devices like CUDA where you must supply the GPU name.
        // This is not necessary for VAAPI, etc.
        if (FFmpeg::av_hwdevice_ctx_create(&hw_device_context, config->device_type, nullptr,
                                           nullptr, 0) < 0) {
            LOG_ERROR(Render, "Failed to create HW device context");
            continue;
        }
        codec_context->hw_device_ctx = FFmpeg::av_buffer_ref(hw_device_context);

        // Get the SW format
        AVHWFramesConstraints* constraints =
            FFmpeg::av_hwdevice_get_hwframe_constraints(hw_device_context, nullptr);
        SCOPE_EXIT({ FFmpeg::av_hwframe_constraints_free(&constraints); });

        if (constraints) {
            sw_pixel_format = constraints->valid_sw_formats ? constraints->valid_sw_formats[0]
                                                            : AV_PIX_FMT_YUV420P;
        } else {
            LOG_WARNING(Render, "Could not query HW device constraints");
            sw_pixel_format = AV_PIX_FMT_YUV420P;
        }

        // For encoders that only need the HW device
        if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) {
            requires_hw_frames = false;
            return true;
        }

        requires_hw_frames = true;

        // Create HW frames context
        AVBufferRef* hw_frames_context_ref;
        SCOPE_EXIT({ FFmpeg::av_buffer_unref(&hw_frames_context_ref); });

        if (!(hw_frames_context_ref = FFmpeg::av_hwframe_ctx_alloc(hw_device_context))) {
            LOG_ERROR(Render, "Failed to create HW frames context");
            continue;
        }

        AVHWFramesContext* hw_frames_context =
            reinterpret_cast<AVHWFramesContext*>(hw_frames_context_ref->data);
        hw_frames_context->format = codec->pix_fmts[i];
        hw_frames_context->sw_format = sw_pixel_format;
        hw_frames_context->width = codec_context->width;
        hw_frames_context->height = codec_context->height;
        hw_frames_context->initial_pool_size = 20; // value from FFmpeg's example

        if (FFmpeg::av_hwframe_ctx_init(hw_frames_context_ref) < 0) {
            LOG_ERROR(Render, "Failed to initialize HW frames context");
            continue;
        }

        codec_context->hw_frames_ctx = FFmpeg::av_buffer_ref(hw_frames_context_ref);
        return true;
    }

    LOG_ERROR(Render, "Failed to find a usable HW pixel format");
    return false;
}

bool FFmpegVideoStream::InitFilters() {
    filter_graph.reset(FFmpeg::avfilter_graph_alloc());

    const AVFilter* source = FFmpeg::avfilter_get_by_name("buffer");
    const AVFilter* sink = FFmpeg::avfilter_get_by_name("buffersink");
    if (!source || !sink) {
        LOG_ERROR(Render, "Could not find buffer source or sink");
        return false;
    }

    // Configure buffer source
    static constexpr AVRational src_time_base{static_cast<int>(VideoCore::FRAME_TICKS),
                                              static_cast<int>(BASE_CLOCK_RATE_ARM11)};
    const std::string in_args =
        fmt::format("video_size={}x{}:pix_fmt={}:time_base={}/{}:pixel_aspect=1", layout.width,
                    layout.height, pixel_format, src_time_base.num, src_time_base.den);
    if (FFmpeg::avfilter_graph_create_filter(&source_context, source, "in", in_args.c_str(),
                                             nullptr, filter_graph.get()) < 0) {
        LOG_ERROR(Render, "Could not create buffer source");
        return false;
    }

    // Configure buffer sink
    if (FFmpeg::avfilter_graph_create_filter(&sink_context, sink, "out", nullptr, nullptr,
                                             filter_graph.get()) < 0) {
        LOG_ERROR(Render, "Could not create buffer sink");
        return false;
    }

    // Point av_opt_set_int_list to correct functions.
#define av_int_list_length_for_size FFmpeg::av_int_list_length_for_size
#define av_opt_set_bin FFmpeg::av_opt_set_bin

    const AVPixelFormat pix_fmts[] = {sw_pixel_format, AV_PIX_FMT_NONE};
    if (av_opt_set_int_list(sink_context, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE,
                            AV_OPT_SEARCH_CHILDREN) < 0) {
        LOG_ERROR(Render, "Could not set output pixel format");
        return false;
    }

    // Initialize filter graph
    // `outputs` as in outputs of the 'previous' graphs
    AVFilterInOut* outputs = FFmpeg::avfilter_inout_alloc();
    outputs->name = FFmpeg::av_strdup("in");
    outputs->filter_ctx = source_context;
    outputs->pad_idx = 0;
    outputs->next = nullptr;

    // `inputs` as in inputs to the 'next' graphs
    AVFilterInOut* inputs = FFmpeg::avfilter_inout_alloc();
    inputs->name = FFmpeg::av_strdup("out");
    inputs->filter_ctx = sink_context;
    inputs->pad_idx = 0;
    inputs->next = nullptr;

    SCOPE_EXIT({
        FFmpeg::avfilter_inout_free(&outputs);
        FFmpeg::avfilter_inout_free(&inputs);
    });

    const std::string filter_graph_desc = fmt::format("fps={}", target_fps);
    if (FFmpeg::avfilter_graph_parse_ptr(filter_graph.get(), filter_graph_desc.c_str(), &inputs,
                                         &outputs, nullptr) < 0) {
        LOG_ERROR(Render, "Could not parse or create filter graph");
        return false;
    }
    if (FFmpeg::avfilter_graph_config(filter_graph.get(), nullptr) < 0) {
        LOG_ERROR(Render, "Could not configure filter graph");
        return false;
    }

    return true;
}

FFmpegAudioStream::~FFmpegAudioStream() {
    Free();
}

bool FFmpegAudioStream::Init(FFmpegMuxer& muxer, bool streaming) {
    InitializeFFmpegLibraries();

    if (!FFmpegStream::Init(muxer)) {
        return false;
    }

    frame_count = 0;

    // Initialize audio codec
    const AVCodec* codec =
        FFmpeg::avcodec_find_encoder_by_name(Settings::values.audio_encoder.c_str());
    codec_context.reset(FFmpeg::avcodec_alloc_context3(codec));
    if (!codec || !codec_context) {
        LOG_ERROR(Render, "Could not find audio encoder or allocate audio codec context");
        return false;
    }

    // Configure audio codec context
    codec_context->codec_type = AVMEDIA_TYPE_AUDIO;
    codec_context->bit_rate = Settings::values.audio_bitrate;
    if (codec->sample_fmts) {
        codec_context->sample_fmt = codec->sample_fmts[0];
    } else {
        codec_context->sample_fmt = AV_SAMPLE_FMT_S16P;
    }

    if (codec->supported_samplerates) {
        codec_context->sample_rate = codec->supported_samplerates[0];
        // Network streams use 48 kHz; some hardware decoders fail on the encoder's first-listed
        // 96 kHz.
        const int preferred_sample_rate = streaming ? 48'000 : AudioCore::native_sample_rate;
        const int* ptr = codec->supported_samplerates;
        while ((*ptr)) {
            if ((*ptr) == preferred_sample_rate) {
                codec_context->sample_rate = preferred_sample_rate;
                break;
            }
            ptr++;
        }
    } else {
        codec_context->sample_rate = AudioCore::native_sample_rate;
    }
    codec_context->time_base.num = 1;
    codec_context->time_base.den = codec_context->sample_rate;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 24, 100) // lavc 59.24.100
    codec_context->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
#else
    codec_context->channel_layout = AV_CH_LAYOUT_STEREO;
    codec_context->channels = 2;
#endif
    if (format_context->oformat->flags & AVFMT_GLOBALHEADER) {
        codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    AVDictionary* options = ToAVDictionary(Settings::values.audio_encoder_options);
    if (FFmpeg::avcodec_open2(codec_context.get(), codec, &options) < 0) {
        LOG_ERROR(Render, "Could not open audio codec");
        return false;
    }

    if (FFmpeg::av_dict_count(options) != 0) { // Successfully set options are removed from the dict
        char* buf = nullptr;
        FFmpeg::av_dict_get_string(options, &buf, ':', ';');
        LOG_WARNING(Render, "Audio encoder options not found: {}", buf);
    }

    if (codec_context->frame_size) {
        frame_size = codec_context->frame_size;
    } else { // variable frame size support
        frame_size = std::tuple_size<AudioCore::StereoFrame16>::value;
    }

    // Create audio stream
    stream = FFmpeg::avformat_new_stream(format_context, codec);
    if (!stream ||
        FFmpeg::avcodec_parameters_from_context(stream->codecpar, codec_context.get()) < 0) {

        LOG_ERROR(Render, "Could not create audio stream");
        return false;
    }
    stream->time_base = codec_context->time_base;
    LOG_INFO(Render, "Audio dumping configured at {} Hz, {} channels", codec_context->sample_rate,
             2);

    // Allocate frame
    audio_frame.reset(FFmpeg::av_frame_alloc());
    audio_frame->format = codec_context->sample_fmt;
    audio_frame->sample_rate = codec_context->sample_rate;

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 24, 100) // lavc 59.24.100
    auto num_channels = codec_context->ch_layout.nb_channels;
    audio_frame->ch_layout = codec_context->ch_layout;
    SwrContext* context = nullptr;
    FFmpeg::swr_alloc_set_opts2(&context, &codec_context->ch_layout, codec_context->sample_fmt,
                                codec_context->sample_rate, &codec_context->ch_layout,
                                AV_SAMPLE_FMT_S16P, AudioCore::native_sample_rate, 0, nullptr);
#else
    auto num_channels = codec_context->channels;
    audio_frame->channel_layout = codec_context->channel_layout;
    audio_frame->channels = num_channels;
    auto* context = FFmpeg::swr_alloc_set_opts(
        nullptr, codec_context->channel_layout, codec_context->sample_fmt,
        codec_context->sample_rate, codec_context->channel_layout, AV_SAMPLE_FMT_S16P,
        AudioCore::native_sample_rate, 0, nullptr);
#endif

    if (!context) {
        LOG_ERROR(Render, "Could not create SWR context");
        return false;
    }
    swr_context.reset(context);
    if (FFmpeg::swr_init(swr_context.get()) < 0) {
        LOG_ERROR(Render, "Could not init SWR context");
        return false;
    }

    // Allocate resampled data
    int error = FFmpeg::av_samples_alloc_array_and_samples(
        &resampled_data, nullptr, num_channels, frame_size, codec_context->sample_fmt, 0);
    if (error < 0) {
        LOG_ERROR(Render, "Could not allocate samples storage");
        return false;
    }

    return true;
}

void FFmpegAudioStream::Free() {
    FFmpegStream::Free();

    audio_frame.reset();
    swr_context.reset();
    // Free resampled data
    if (resampled_data) {
        FFmpeg::av_freep(&resampled_data[0]);
    }
    FFmpeg::av_freep(&resampled_data);
}

void FFmpegAudioStream::ProcessFrame(const VariableAudioFrame& channel0,
                                     const VariableAudioFrame& channel1) {
    ASSERT_MSG(channel0.size() == channel1.size(),
               "Frames of the two channels must have the same number of samples");

    const auto sample_size = FFmpeg::av_get_bytes_per_sample(codec_context->sample_fmt);
    std::array<const u8*, 2> src_data = {reinterpret_cast<const u8*>(channel0.data()),
                                         reinterpret_cast<const u8*>(channel1.data())};

    std::array<u8*, 2> dst_data;
    if (FFmpeg::av_sample_fmt_is_planar(codec_context->sample_fmt)) {
        dst_data = {resampled_data[0] + sample_size * offset,
                    resampled_data[1] + sample_size * offset};
    } else {
        dst_data = {resampled_data[0] + sample_size * offset * 2}; // 2 channels
    }

    auto resampled_count =
        FFmpeg::swr_convert(swr_context.get(), dst_data.data(), frame_size - offset,
                            src_data.data(), static_cast<int>(channel0.size()));
    if (resampled_count < 0) {
        LOG_ERROR(Render, "Audio frame dropped: Could not resample data");
        return;
    }

    offset += resampled_count;
    if (offset < frame_size) { // Still not enough to form a frame
        return;
    }

    while (true) {
        // Prepare frame
        audio_frame->nb_samples = frame_size;
        audio_frame->data[0] = resampled_data[0];
        if (FFmpeg::av_sample_fmt_is_planar(codec_context->sample_fmt)) {
            audio_frame->data[1] = resampled_data[1];
        }
        audio_frame->pts = frame_count * frame_size;
        frame_count++;

        SendFrame(audio_frame.get());

        // swr_convert buffers input internally. Try to get more resampled data
        resampled_count =
            FFmpeg::swr_convert(swr_context.get(), resampled_data, frame_size, nullptr, 0);
        if (resampled_count < 0) {
            LOG_ERROR(Render, "Audio frame dropped: Could not resample data");
            return;
        }
        if (resampled_count < frame_size) {
            offset = resampled_count;
            break;
        }
    }
}

void FFmpegAudioStream::Flush() {
    // Send the last samples
    audio_frame->nb_samples = offset;
    audio_frame->data[0] = resampled_data[0];
    if (FFmpeg::av_sample_fmt_is_planar(codec_context->sample_fmt)) {
        audio_frame->data[1] = resampled_data[1];
    }
    audio_frame->pts = frame_count * frame_size;

    SendFrame(audio_frame.get());

    FFmpegStream::Flush();
}

FFmpegMuxer::~FFmpegMuxer() {
    Free();
}

bool FFmpegMuxer::Init(const std::string& path, const Layout::FramebufferLayout& layout,
                       bool enable_audio, u32 target_fps, bool low_latency) {
    InitializeFFmpegLibraries();

    const bool network_output = path.starts_with("udp://");
    const bool use_low_latency = network_output || low_latency;
    if (!network_output && !FileUtil::CreateFullPath(path)) {
        return false;
    }

    // Get output format
    const auto format = network_output ? std::string{"mpegts"} : Settings::values.output_format;
    auto* output_format = FFmpeg::av_guess_format(format.c_str(), path.c_str(), nullptr);
    if (!output_format) {
        LOG_ERROR(Render, "Could not get format {}", format);
        return false;
    }

    // Initialize format context
    auto* format_context_raw = format_context.get();
    if (FFmpeg::avformat_alloc_output_context2(&format_context_raw, output_format, nullptr,
                                               path.c_str()) < 0) {
        LOG_ERROR(Render, "Could not allocate output context");
        return false;
    }
    format_context.reset(format_context_raw);
    if (use_low_latency) {
        format_context->flags |= AVFMT_FLAG_FLUSH_PACKETS;
    }

    if (!video_stream.Init(*this, layout, target_fps, use_low_latency))
        return false;
    audio_enabled = enable_audio;
    if (audio_enabled && !audio_stream.Init(*this, use_low_latency))
        return false;

    AVDictionary* options = ToAVDictionary(Settings::values.format_options);
    // Open video file
    if (FFmpeg::avio_open(&format_context->pb, path.c_str(), AVIO_FLAG_WRITE) < 0 ||
        FFmpeg::avformat_write_header(format_context.get(), &options)) {

        LOG_ERROR(Render, "Could not open {}", path);
        return false;
    }
    if (FFmpeg::av_dict_count(options) != 0) { // Successfully set options are removed from the dict
        char* buf = nullptr;
        FFmpeg::av_dict_get_string(options, &buf, ':', ';');
        LOG_WARNING(Render, "Format options not found: {}", buf);
    }

    LOG_INFO(Render, "Dumping frames to {} ({}x{})", path, layout.width, layout.height);
    return true;
}

void FFmpegMuxer::Free() {
    video_stream.Free();
    audio_stream.Free();
    format_context.reset();
}

void FFmpegMuxer::ProcessVideoFrame(VideoFrame& frame) {
    video_stream.ProcessFrame(frame);
}

void FFmpegMuxer::ProcessAudioFrame(const VariableAudioFrame& channel0,
                                    const VariableAudioFrame& channel1) {
    if (audio_enabled)
        audio_stream.ProcessFrame(channel0, channel1);
}

void FFmpegMuxer::FlushVideo() {
    video_stream.Flush();
}

void FFmpegMuxer::FlushAudio() {
    if (audio_enabled)
        audio_stream.Flush();
}

void FFmpegMuxer::WriteTrailer() {
    std::scoped_lock lock{format_context_mutex};
    FFmpeg::av_interleaved_write_frame(format_context.get(), nullptr);
    FFmpeg::av_write_trailer(format_context.get());
}

FFmpegBackend::FFmpegBackend(VideoCore::RendererBase& renderer_, bool manage_renderer_capture_,
                             bool video_only_)
    : FFmpegBackend(renderer_,
                    FFmpegBackendConfig{manage_renderer_capture_, !video_only_, 60, false}) {}

FFmpegBackend::FFmpegBackend(VideoCore::RendererBase& renderer_, FFmpegBackendConfig config)
    : renderer{renderer_}, manage_renderer_capture{config.manage_renderer_capture},
      audio_enabled{config.audio_enabled}, target_fps{std::clamp(config.target_fps, 1U, 60U)},
      low_latency{config.low_latency} {}

FFmpegBackend::~FFmpegBackend() {
    ASSERT_MSG(!IsDumping(), "Dumping must be stopped first");

    if (video_processing_thread.joinable())
        video_processing_thread.join();
    if (audio_processing_thread.joinable())
        audio_processing_thread.join();
    ffmpeg.Free();
}

bool FFmpegBackend::StartDumping(const std::string& path, const Layout::FramebufferLayout& layout) {
    InitializeFFmpegLibraries();

    if (!ffmpeg.Init(path, layout, audio_enabled, target_fps, low_latency)) {
        ffmpeg.Free();
        return false;
    }

    video_layout = layout;

    if (video_processing_thread.joinable()) {
        video_processing_thread.join();
    }
    if (audio_processing_thread.joinable()) {
        audio_processing_thread.join();
    }
    event1.Reset();
    event2.Reset();
    processing_ended.Reset();
    current_buffer = 0;
    next_buffer = 1;
    for (auto& queue : audio_frame_queues) {
        queue.Clear();
    }

    if (audio_enabled) {
        audio_processing_thread = std::thread([&] {
            VariableAudioFrame channel0, channel1;
            while (true) {
                channel0 = audio_frame_queues[0].PopWait();
                channel1 = audio_frame_queues[1].PopWait();
                if (channel0.empty()) {
                    // An empty frame marks the end of frame data
                    ffmpeg.FlushAudio();
                    break;
                }
                ffmpeg.ProcessAudioFrame(channel0, channel1);
            }
        });
    }

    video_processing_thread = std::thread([&] {
        event1.Set();
        while (true) {
            event2.Wait();
            current_buffer = (current_buffer + 1) % 2;
            next_buffer = (current_buffer + 1) % 2;
            event1.Set();
            // Process this frame
            auto& frame = video_frame_buffers[current_buffer];
            if (frame.width == 0 && frame.height == 0) {
                // An empty frame marks the end of frame data
                ffmpeg.FlushVideo();
                break;
            }
            ffmpeg.ProcessVideoFrame(frame);
        }
        // Finish audio execution first if not done yet
        if (audio_processing_thread.joinable())
            audio_processing_thread.join();
        EndDumping();
    });

    if (manage_renderer_capture)
        renderer.PrepareVideoDumping();
    is_dumping = true;

    return true;
}

void FFmpegBackend::AddVideoFrame(VideoFrame frame) {
    event1.Wait();
    video_frame_buffers[next_buffer] = std::move(frame);
    event2.Set();
}

void FFmpegBackend::AddAudioFrame(AudioCore::StereoFrame16 frame) {
    if (!audio_enabled)
        return;
    std::scoped_lock lock{audio_submission_mutex};
    if (!IsDumping())
        return;
    std::array<VariableAudioFrame, 2> refactored_frame;
    for (auto& channel : refactored_frame) {
        channel.resize(frame.size());
    }
    for (std::size_t i = 0; i < frame.size(); i++) {
        refactored_frame[0][i] = frame[i][0];
        refactored_frame[1][i] = frame[i][1];
    }

    audio_frame_queues[0].Push(std::move(refactored_frame[0]));
    audio_frame_queues[1].Push(std::move(refactored_frame[1]));
}

void FFmpegBackend::AddAudioSample(const std::array<s16, 2>& sample) {
    if (!audio_enabled)
        return;
    std::scoped_lock lock{audio_submission_mutex};
    if (!IsDumping())
        return;
    audio_frame_queues[0].Push(VariableAudioFrame{sample[0]});
    audio_frame_queues[1].Push(VariableAudioFrame{sample[1]});
}

void FFmpegBackend::StopDumping() {
    is_dumping = false;
    if (manage_renderer_capture)
        renderer.CleanupVideoDumping();

    // Flush the video processing queue
    AddVideoFrame(VideoFrame());
    if (audio_enabled) {
        std::scoped_lock lock{audio_submission_mutex};
        for (auto i : {0, 1}) {
            // Flush the audio processing queue
            audio_frame_queues[i].Push(VariableAudioFrame());
        }
    }
    // Wait until processing ends
    processing_ended.Wait();
}

bool FFmpegBackend::IsDumping() const {
    return is_dumping.load(std::memory_order_relaxed);
}

Layout::FramebufferLayout FFmpegBackend::GetLayout() const {
    return video_layout;
}

void FFmpegBackend::EndDumping() {
    LOG_INFO(Render, "Ending frame dumping");

    ffmpeg.WriteTrailer();
    ffmpeg.Free();
    processing_ended.Set();
}

// To std string, but handles nullptr
std::string ToStdString(const char* str, const std::string& fallback = "") {
    return str ? std::string{str} : fallback;
}

std::string FormatDuration(s64 duration) {
    // The following is implemented according to libavutil code (opt.c)
    std::string out;
    if (duration < 0 && duration != std::numeric_limits<s64>::min()) {
        out.append("-");
        duration = -duration;
    }
    if (duration == std::numeric_limits<s64>::max()) {
        return "INT64_MAX";
    } else if (duration == std::numeric_limits<s64>::min()) {
        return "INT64_MIN";
    } else if (duration > 3600ll * 1000000ll) {
        out.append(fmt::format("{}:{:02d}:{:02d}.{:06d}", duration / 3600000000ll,
                               ((duration / 60000000ll) % 60), ((duration / 1000000ll) % 60),
                               duration % 1000000));
    } else if (duration > 60ll * 1000000ll) {
        out.append(fmt::format("{}:{:02d}.{:06d}", duration / 60000000ll,
                               ((duration / 1000000ll) % 60), duration % 1000000));
    } else {
        out.append(fmt::format("{}.{:06d}", duration / 1000000ll, duration % 1000000));
    }
    while (out.back() == '0') {
        out.erase(out.size() - 1, 1);
    }
    if (out.back() == '.') {
        out.erase(out.size() - 1, 1);
    }
    return out;
}

std::string FormatDefaultValue(const AVOption* option,
                               std::span<const OptionInfo::NamedConstant> named_constants) {
    // The following is taken and modified from libavutil code (opt.c)
    switch (option->type) {
    case AV_OPT_TYPE_BOOL: {
        const auto value = option->default_val.i64;
        if (value < 0) {
            return "auto";
        }
        return value ? "true" : "false";
    }
    case AV_OPT_TYPE_FLAGS: {
        const auto value = option->default_val.i64;
        std::string out;
        for (const auto& constant : named_constants) {
            if (!(value & constant.value)) {
                continue;
            }
            if (!out.empty()) {
                out.append("+");
            }
            out.append(constant.name);
        }
        return out.empty() ? fmt::format("{}", value) : out;
    }
    case AV_OPT_TYPE_DURATION: {
        return FormatDuration(option->default_val.i64);
    }
    case AV_OPT_TYPE_INT:
    case AV_OPT_TYPE_UINT64:
    case AV_OPT_TYPE_INT64: {
        const auto value = option->default_val.i64;
        for (const auto& constant : named_constants) {
            if (constant.value == value) {
                return constant.name;
            }
        }
        return fmt::format("{}", value);
    }
    case AV_OPT_TYPE_DOUBLE:
    case AV_OPT_TYPE_FLOAT: {
        return fmt::format("{}", option->default_val.dbl);
    }
    case AV_OPT_TYPE_RATIONAL: {
        const auto q = FFmpeg::av_d2q(option->default_val.dbl, std::numeric_limits<int>::max());
        return fmt::format("{}/{}", q.num, q.den);
    }
    case AV_OPT_TYPE_PIXEL_FMT: {
        const char* name =
            FFmpeg::av_get_pix_fmt_name(static_cast<AVPixelFormat>(option->default_val.i64));
        return ToStdString(name, "none");
    }
    case AV_OPT_TYPE_SAMPLE_FMT: {
        const char* name =
            FFmpeg::av_get_sample_fmt_name(static_cast<AVSampleFormat>(option->default_val.i64));
        return ToStdString(name, "none");
    }
    case AV_OPT_TYPE_COLOR:
    case AV_OPT_TYPE_IMAGE_SIZE:
    case AV_OPT_TYPE_STRING:
    case AV_OPT_TYPE_DICT:
    case AV_OPT_TYPE_VIDEO_RATE: {
        return ToStdString(option->default_val.str);
    }
    case AV_OPT_TYPE_CHANNEL_LAYOUT: {
        return fmt::format("{:#x}", option->default_val.i64);
    }
    default:
        return "";
    }
}

void GetOptionListSingle(std::vector<OptionInfo>& out, const AVClass* av_class) {
    if (av_class == nullptr) {
        return;
    }

    const AVOption* current = nullptr;
    std::unordered_map<std::string, std::vector<OptionInfo::NamedConstant>> named_constants_map;
    // First iteration: find and place all named constants
    while ((current = FFmpeg::av_opt_next(&av_class, current))) {
        if (current->type != AV_OPT_TYPE_CONST || !current->unit) {
            continue;
        }
        named_constants_map[current->unit].push_back(
            {current->name, ToStdString(current->help), current->default_val.i64});
    }
    // Second iteration: find all options
    current = nullptr;
    while ((current = FFmpeg::av_opt_next(&av_class, current))) {
        // Currently we cannot handle binary options
        if (current->type == AV_OPT_TYPE_CONST || current->type == AV_OPT_TYPE_BINARY) {
            continue;
        }
        std::vector<OptionInfo::NamedConstant> named_constants;
        if (current->unit && named_constants_map.count(current->unit)) {
            named_constants = named_constants_map.at(current->unit);
        }
        const auto default_value = FormatDefaultValue(current, named_constants);
        out.push_back({current->name, ToStdString(current->help), current->type, default_value,
                       std::move(named_constants), current->min, current->max});
    }
}

void GetOptionList(std::vector<OptionInfo>& out, const AVClass* av_class, bool search_children) {
    if (av_class == nullptr) {
        return;
    }

    GetOptionListSingle(out, av_class);

    if (!search_children) {
        return;
    }

    const AVClass* child_class = nullptr;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(56, 53, 100) // lavu 56.53.100
    void* iter = nullptr;
    while ((child_class = FFmpeg::av_opt_child_class_iterate(av_class, &iter))) {
#else
    while ((child_class = FFmpeg::av_opt_child_class_next(av_class, child_class))) {
#endif
        GetOptionListSingle(out, child_class);
    }
}

std::vector<OptionInfo> GetOptionList(const AVClass* av_class, bool search_children) {
    std::vector<OptionInfo> out;
    GetOptionList(out, av_class, search_children);
    return out;
}

std::vector<EncoderInfo> ListEncoders(AVMediaType type) {
    InitializeFFmpegLibraries();

    std::vector<EncoderInfo> out;

    const AVCodec* current = nullptr;
    void* data = nullptr; // For libavcodec to save the iteration state
    while ((current = FFmpeg::av_codec_iterate(&data))) {
        if (!FFmpeg::av_codec_is_encoder(current) || current->type != type) {
            continue;
        }
        out.push_back({current->name, ToStdString(current->long_name), current->id,
                       GetOptionList(current->priv_class, true)});
    }
    return out;
}

std::vector<OptionInfo> GetEncoderGenericOptions() {
    return GetOptionList(FFmpeg::avcodec_get_class(), false);
}

std::vector<FormatInfo> ListFormats() {
    InitializeFFmpegLibraries();

    std::vector<FormatInfo> out;

    const AVOutputFormat* current = nullptr;
    void* data = nullptr; // For libavformat to save the iteration state
    while ((current = FFmpeg::av_muxer_iterate(&data))) {
        const auto extensions = Common::SplitString(ToStdString(current->extensions), ',');

        std::set<AVCodecID> supported_video_codecs;
        std::set<AVCodecID> supported_audio_codecs;
        // Go through all codecs
        const AVCodecDescriptor* codec = nullptr;
        while ((codec = FFmpeg::avcodec_descriptor_next(codec))) {
            if (FFmpeg::avformat_query_codec(current, codec->id, FF_COMPLIANCE_NORMAL) == 1) {
                if (codec->type == AVMEDIA_TYPE_VIDEO) {
                    supported_video_codecs.emplace(codec->id);
                } else if (codec->type == AVMEDIA_TYPE_AUDIO) {
                    supported_audio_codecs.emplace(codec->id);
                }
            }
        }

        if (supported_video_codecs.empty() || supported_audio_codecs.empty()) {
            continue;
        }

        out.push_back({current->name, ToStdString(current->long_name), extensions,
                       std::move(supported_video_codecs), std::move(supported_audio_codecs),
                       GetOptionList(current->priv_class, true)});
    }
    return out;
}

std::vector<OptionInfo> GetFormatGenericOptions() {
    return GetOptionList(FFmpeg::avformat_get_class(), false);
}

std::vector<std::string> GetPixelFormats() {
    std::vector<std::string> out;
    const AVPixFmtDescriptor* current = nullptr;
    while ((current = FFmpeg::av_pix_fmt_desc_next(current))) {
        out.emplace_back(current->name);
    }
    return out;
}

std::vector<std::string> GetSampleFormats() {
    std::vector<std::string> out;
    for (int current = AV_SAMPLE_FMT_U8; current < AV_SAMPLE_FMT_NB; current++) {
        out.emplace_back(FFmpeg::av_get_sample_fmt_name(static_cast<AVSampleFormat>(current)));
    }
    return out;
}

} // namespace VideoDumper
