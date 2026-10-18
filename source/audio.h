
#ifndef AUDIO_H
#define AUDIO_H

#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>

typedef struct {
    AVCodecContext *codec;
    SwrContext *resampler;
    int sampleRate;
    int channels;
} AudioDecoder;

int audio_init(AudioDecoder *audio, AVCodecParameters *params);

int audio_decode(AudioDecoder *audio, AVPacket *packet);

void audio_close(AudioDecoder *audio);

#endif
