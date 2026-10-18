
#include "audio.h"

#include <stdlib.h>
#include <3ds.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>

int audio_init(AudioDecoder *audio, AVCodecParameters *params)
{
    if (!audio || !params)
        return -1;

    audio->codec = NULL;
    audio->resampler = NULL;
    audio->sampleRate = 44100;
    audio->channels = 2;

    const AVCodec *decoder = avcodec_find_decoder(params->codec_id);
    if (!decoder)
        return -1;

    audio->codec = avcodec_alloc_context3(decoder);
    if (!audio->codec)
        return -1;

    if (avcodec_parameters_to_context(audio->codec, params) < 0)
        goto fail;

    audio->codec->thread_count = 1;

    if (avcodec_open2(audio->codec, decoder, NULL) < 0)
        goto fail;

    AVChannelLayout outputLayout = AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout inputLayout = {0};

    if (audio->codec->ch_layout.nb_channels > 0) {
        if (av_channel_layout_copy(
                &inputLayout, &audio->codec->ch_layout) < 0) {
            av_channel_layout_uninit(&outputLayout);
            goto fail;
        }
    } else {
        av_channel_layout_default(&inputLayout, 2);
    }

    int ret = swr_alloc_set_opts2(
        &audio->resampler,
        &outputLayout, AV_SAMPLE_FMT_S16, 44100,
        &inputLayout, audio->codec->sample_fmt,
        audio->codec->sample_rate, 0, NULL);

    av_channel_layout_uninit(&inputLayout);
    av_channel_layout_uninit(&outputLayout);

    if (ret < 0 || !audio->resampler ||
        swr_init(audio->resampler) < 0)
        goto fail;

    return 0;

fail:
    audio_close(audio);
    return -1;
}

/*
 * Envoie un paquet au décodeur.
 * La récupération des frames et leur mise en file NDSP
 * restent à raccorder au lecteur principal.
 */
int audio_decode(AudioDecoder *audio, AVPacket *packet)
{
    if (!audio || !audio->codec || !packet)
        return -1;

    return avcodec_send_packet(audio->codec, packet);
}

void audio_close(AudioDecoder *audio)
{
    if (!audio)
        return;

    swr_free(&audio->resampler);
    avcodec_free_context(&audio->codec);
}
