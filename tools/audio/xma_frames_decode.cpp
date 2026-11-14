// xenon_xma_frames_decode <frames> <sample-rate> <channels> <out.pcm>
//
// Decodes XMA frames, in the order the file lists them, with FFmpeg's
// XMAFRAMES decoder - the decoder Xenon's XMA engine uses - and writes each
// frame's 512 samples as 16-bit big-endian interleaved PCM, the layout the
// engine writes to a context's output ring.
//
// <frames> comes from tools/audio/xma_stream_frames.py --frames: the frames
// of a dumped stream (XENON_XMA_DUMP) found by walking its packets straight
// through. The output is the stream as it should sound with every frame
// decoded once, in order; matching the engine's .pcm against it frame by
// frame (xma_frame_order.py) shows any frame the engine skipped, repeated or
// took out of order.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

namespace {

std::int16_t to_s16(float value) {
  if (value > 1.0f) value = 1.0f;
  if (value < -1.0f) value = -1.0f;
  return static_cast<std::int16_t>(value * 32767.0f + (value >= 0.0f ? 0.5f : -0.5f));
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 5) {
    std::cerr << "usage: xenon_xma_frames_decode <frames> <sample-rate> <channels> <out.pcm>\n";
    return 2;
  }
  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::cerr << "cannot read " << argv[1] << "\n";
    return 1;
  }
  const std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
  const int sample_rate = std::atoi(argv[2]);
  const int channels = std::atoi(argv[3]);
  if (sample_rate <= 0 || (channels != 1 && channels != 2)) {
    std::cerr << "sample rate must be positive and channels 1 or 2\n";
    return 2;
  }

  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
  if (!codec) {
    std::cerr << "this FFmpeg has no XMAFRAMES decoder\n";
    return 1;
  }
  AVCodecContext* context = avcodec_alloc_context3(codec);
  context->sample_rate = sample_rate;
#if defined(XENON_AUDIO_FFMPEG_HAS_CH_LAYOUT)
  av_channel_layout_default(&context->ch_layout, channels);
#else
  context->channels = channels;
#endif
  // XMA_SKIP_MANUAL=1 opens the decoder as xenia/rexglue do
  // (AV_CODEC_FLAG2_SKIP_MANUAL), for comparing their output with Xenon's.
  if (const char* manual = std::getenv("XMA_SKIP_MANUAL"); manual && *manual == '1') {
    context->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;
  }
  if (avcodec_open2(context, codec, nullptr) < 0) {
    std::cerr << "cannot open the XMAFRAMES decoder\n";
    return 1;
  }
  AVPacket* packet = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  std::vector<std::uint8_t> out;
  std::size_t frames = 0, rejected = 0;
  for (std::size_t offset = 0; offset + 4 <= file.size();) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, file.data() + offset, 4);
    offset += 4;
    const std::size_t bytes = (bits + 7u) / 8u;
    if (offset + bytes > file.size()) break;
    // As the engine builds it: one byte with the unused bits at the end of
    // the last byte (bits 4..2), then the frame from bit 0.
    if (av_new_packet(packet, static_cast<int>(bytes + 1)) < 0) return 1;
    packet->data[0] = static_cast<std::uint8_t>(((8u - (bits & 7u)) & 7u) << 2u);
    std::memcpy(packet->data + 1, file.data() + offset, bytes);
    offset += bytes;
    ++frames;
    bool decoded = avcodec_send_packet(context, packet) >= 0 &&
                   avcodec_receive_frame(context, frame) >= 0 && frame->nb_samples >= 512;
    av_packet_unref(packet);
    if (!decoded) ++rejected;
    for (int sample = 0; sample < 512; ++sample) {
      for (int ch = 0; ch < channels; ++ch) {
        const float value =
            decoded ? reinterpret_cast<const float*>(frame->extended_data[ch])[sample] : 0.0f;
        const auto s16 = static_cast<std::uint16_t>(to_s16(value));
        out.push_back(static_cast<std::uint8_t>(s16 >> 8));
        out.push_back(static_cast<std::uint8_t>(s16 & 0xFF));
      }
    }
    av_frame_unref(frame);
  }
  std::ofstream(argv[4], std::ios::binary)
      .write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
  std::cout << frames << " frames, " << rejected << " rejected by the decoder\n";
  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&context);
  return 0;
}
