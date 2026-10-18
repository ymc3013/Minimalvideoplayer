
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#define VIDEO_PATH "sdmc:/Movies/test.avi"
#define SCREEN_W 400
#define SCREEN_H 240
#define AUDIO_RATE 44100
#define AUDIO_CHANNELS 2
#define AUDIO_BUFFER_SAMPLES 4096
#define AUDIO_BUFFER_COUNT 4

static ndspWaveBuf audioBuffers[AUDIO_BUFFER_COUNT];
static int16_t *audioMemory[AUDIO_BUFFER_COUNT];
static int audioIndex = 0;

static void show_error(const char *message, int error)
{
    char text[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(error, text, sizeof(text));

    consoleInit(GFX_TOP, NULL);
    printf("%s\n%s\n", message, text);
    printf("\nSTART pour quitter.");

    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START)
            break;
        gspWaitForVBlank();
    }
}

static void display_frame(struct SwsContext *scaler,
                          AVFrame *frame,
                          uint8_t *rgb,
                          int rgbStride)
{
    uint8_t *destData[4] = { rgb, NULL, NULL, NULL };
    int destStride[4] = { rgbStride, 0, 0, 0 };

    sws_scale(scaler,
              (const uint8_t * const *)frame->data,
              frame->linesize,
              0,
              frame->height,
              destData,
              destStride);

    u16 width = 0, height = 0;
    u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT,
                               &width, &height);

    if (!fb || width != SCREEN_W || height != SCREEN_H)
        return;

    /*
     * Le framebuffer de la 3DS est organisé par colonnes,
     * avec l'axe vertical inversé.
     */
    for (int y = 0; y < SCREEN_H; y++) {
        for (int x = 0; x < SCREEN_W; x++) {
            const uint8_t *pixel =
                &rgb[y * rgbStride + x * 3];

            unsigned int pos =
                ((SCREEN_H - 1 - y) * SCREEN_W + x) * 3;

            fb[pos + 0] = pixel[0];
            fb[pos + 1] = pixel[1];
            fb[pos + 2] = pixel[2];
        }
    }

    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

static void queue_audio(const uint8_t *data, int samples)
{
    if (samples <= 0)
        return;

    ndspWaveBuf *buf = &audioBuffers[audioIndex];

    while (aptMainLoop() && buf->status == NDSP_WBUF_PLAYING) {
        gspWaitForVBlank();
    }

    if (!aptMainLoop())
        return;

    size_t bytes = (size_t)samples * AUDIO_CHANNELS * sizeof(int16_t);

    memcpy(audioMemory[audioIndex], data, bytes);

    memset(buf, 0, sizeof(*buf));
    buf->data_vaddr = audioMemory[audioIndex];
    buf->nsamples = samples;
    buf->looping = false;

    DSP_FlushDataCache(audioMemory[audioIndex], bytes);
    ndspChnWaveBufAdd(0, buf);

    audioIndex = (audioIndex + 1) % AUDIO_BUFFER_COUNT;
}

int main(void)
{
    AVFormatContext *format = NULL;
    AVCodecContext *videoCodec = NULL;
    AVCodecContext *audioCodec = NULL;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    SwsContext *scaler = NULL;
    SwrContext *resampler = NULL;

    uint8_t *rgb = NULL;
    uint8_t *audioConverted = NULL;

    int videoStream = -1;
    int audioStream = -1;
    int result = 0;
    int ndspStarted = 0;

    gfxInitDefault();
    gfxSet3D(false);

    for (int i = 0; i < AUDIO_BUFFER_COUNT; i++) {
        audioMemory[i] = linearAlloc(
            AUDIO_BUFFER_SAMPLES * AUDIO_CHANNELS * sizeof(int16_t));

        if (!audioMemory[i]) {
            result = AVERROR(ENOMEM);
            goto cleanup;
        }
    }

    ndspInit();
    ndspStarted = 1;

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, AUDIO_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);

    result = avformat_open_input(&format, VIDEO_PATH, NULL, NULL);
    if (result < 0)
        goto error;

    result = avformat_find_stream_info(format, NULL);
    if (result < 0)
        goto error;

    for (unsigned int i = 0; i < format->nb_streams; i++) {
        AVStream *stream = format->streams[i];

        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            videoStream < 0)
            videoStream = (int)i;

        if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
            audioStream < 0)
            audioStream = (int)i;
    }

    if (videoStream < 0) {
        result = AVERROR_STREAM_NOT_FOUND;
        goto error;
    }

    {
        AVCodecParameters *params =
            format->streams[videoStream]->codecpar;
        const AVCodec *decoder =
            avcodec_find_decoder(params->codec_id);

        if (!decoder) {
            result = AVERROR_DECODER_NOT_FOUND;
            goto error;
        }

        videoCodec = avcodec_alloc_context3(decoder);
        if (!videoCodec) {
            result = AVERROR(ENOMEM);
            goto error;
        }

        result = avcodec_parameters_to_context(videoCodec, params);
        if (result < 0)
            goto error;

        videoCodec->thread_count = 1;

        result = avcodec_open2(videoCodec, decoder, NULL);
        if (result < 0)
            goto error;
    }

    scaler = sws_getContext(
        videoCodec->width,
        videoCodec->height,
        videoCodec->pix_fmt,
        SCREEN_W,
        SCREEN_H,
        AV_PIX_FMT_RGB24,
        SWS_BILINEAR,
        NULL, NULL, NULL);

    if (!scaler) {
        result = AVERROR(ENOMEM);
        goto error;
    }

    rgb = malloc(SCREEN_W * SCREEN_H * 3);
    if (!rgb) {
        result = AVERROR(ENOMEM);
        goto error;
    }

    frame = av_frame_alloc();
    packet = av_packet_alloc();

    if (!frame || !packet) {
        result = AVERROR(ENOMEM);
        goto error;
    }

    if (audioStream >= 0) {
        AVCodecParameters *params =
            format->streams[audioStream]->codecpar;
        const AVCodec *decoder =
            avcodec_find_decoder(params->codec_id);

        if (decoder) {
            audioCodec = avcodec_alloc_context3(decoder);

            if (audioCodec) {
                result = avcodec_parameters_to_context(
                    audioCodec, params);

                if (result >= 0)
                    result = avcodec_open2(
                        audioCodec, decoder, NULL);

                if (result < 0) {
                    avcodec_free_context(&audioCodec);
                    result = 0;
                }
            }

            if (audioCodec) {
                AVChannelLayout outputLayout =
                    AV_CHANNEL_LAYOUT_STEREO;

                AVChannelLayout inputLayout = {0};

                if (av_channel_layout_copy(
                        &inputLayout,
                        &audioCodec->ch_layout) < 0) {
                    av_channel_layout_default(
                        &inputLayout,
                        audioCodec->ch_layout.nb_channels > 0
                            ? audioCodec->ch_layout.nb_channels
                            : 2);
                }

                result = swr_alloc_set_opts2(
                    &resampler,
                    &outputLayout,
                    AV_SAMPLE_FMT_S16,
                    AUDIO_RATE,
                    &inputLayout,
                    audioCodec->sample_fmt,
                    audioCodec->sample_rate,
                    0,
                    NULL);

                av_channel_layout_uninit(&inputLayout);
                av_channel_layout_uninit(&outputLayout);

                if (result < 0 || !resampler ||
                    swr_init(resampler) < 0) {
                    swr_free(&resampler);
                    avcodec_free_context(&audioCodec);
                    result = 0;
                }
            }
        }
    }

    while (aptMainLoop() &&
           av_read_frame(format, packet) >= 0) {

        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            av_packet_unref(packet);
            break;
        }

        if (packet->stream_index == videoStream) {
            result = avcodec_send_packet(videoCodec, packet);

            if (result >= 0) {
                while (avcodec_receive_frame(
                           videoCodec, frame) >= 0) {
                    display_frame(
                        scaler, frame, rgb, SCREEN_W * 3);
                    av_frame_unref(frame);
                }
            }
        } else if (audioCodec && resampler &&
                   packet->stream_index == audioStream) {

            result = avcodec_send_packet(audioCodec, packet);

            if (result >= 0) {
                while (avcodec_receive_frame(
                           audioCodec, frame) >= 0) {

                    int capacity = (int)av_rescale_rnd(
                        swr_get_delay(resampler,
                                      audioCodec->sample_rate) +
                            frame->nb_samples,
                        AUDIO_RATE,
                        audioCodec->sample_rate,
                        AV_ROUND_UP);

                    if (capacity > 0 &&
                        capacity <= AUDIO_BUFFER_SAMPLES) {

                        audioConverted = malloc(
                            (size_t)capacity *
                            AUDIO_CHANNELS * sizeof(int16_t));

                        if (audioConverted) {
                            uint8_t *out[] = {
                                audioConverted
                            };

                            int converted = swr_convert(
                                resampler,
                                out,
                                capacity,
                                (const uint8_t **)frame->extended_data,
                                frame->nb_samples);

                            if (converted > 0)
                                queue_audio(
                                    audioConverted, converted);

                            free(audioConverted);
                            audioConverted = NULL;
                        }
                    }

                    av_frame_unref(frame);
                }
            }
        }

        av_packet_unref(packet);
    }

    goto cleanup;

error:
    show_error("Erreur de lecture du fichier AVI :", result);

cleanup:
    free(audioConverted);
    free(rgb);

    sws_freeContext(scaler);
    swr_free(&resampler);
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&videoCodec);
    avcodec_free_context(&audioCodec);
    avformat_close_input(&format);

    if (ndspStarted)
        ndspExit();

    for (int i = 0; i < AUDIO_BUFFER_COUNT; i++) {
        if (audioMemory[i])
            linearFree(audioMemory[i]);
    }

    gfxExit();
    return 0;
}
