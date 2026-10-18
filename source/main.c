#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <limits.h>

#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#define VIDEO_PATH "sdmc:/Movies/test.avi"
#define SCREEN_W 400
#define SCREEN_H 240
#define FB_WIDTH 240
#define FB_HEIGHT 400

#define AUDIO_RATE 44100
#define AUDIO_CHANNELS 2
#define AUDIO_BUFFER_SAMPLES 4096
#define AUDIO_BUFFER_COUNT 4
#define IO_BUFFER_SIZE 32768

static ndspWaveBuf audioBuffers[AUDIO_BUFFER_COUNT];
static int16_t *audioMemory[AUDIO_BUFFER_COUNT];
static int audioIndex = 0;

static int read_file_packet(
    void *opaque,
    uint8_t *buffer,
    int buffer_size)
{
    FILE *file = (FILE *)opaque;

    if (buffer_size <= 0)
        return AVERROR(EINVAL);

    size_t count = fread(buffer, 1, (size_t)buffer_size, file);

    if (count > 0)
        return (int)count;

    if (feof(file))
        return AVERROR_EOF;

    return AVERROR(errno ? errno : EIO);
}

static int64_t seek_file_packet(
    void *opaque,
    int64_t offset,
    int whence)
{
    FILE *file = (FILE *)opaque;
    long current;
    long end;
    int origin;

    if (whence == AVSEEK_SIZE) {
        current = ftell(file);

        if (current < 0)
            return AVERROR(errno ? errno : EIO);

        if (fseek(file, 0, SEEK_END) != 0)
            return AVERROR(errno ? errno : EIO);

        end = ftell(file);

        if (fseek(file, current, SEEK_SET) != 0)
            return AVERROR(errno ? errno : EIO);

        if (end < 0)
            return AVERROR(errno ? errno : EIO);

        return (int64_t)end;
    }

    whence &= ~AVSEEK_FORCE;

    switch (whence) {
    case SEEK_SET:
        origin = SEEK_SET;
        break;

    case SEEK_CUR:
        origin = SEEK_CUR;
        break;

    case SEEK_END:
        origin = SEEK_END;
        break;

    default:
        return AVERROR(EINVAL);
    }

    if (offset > LONG_MAX || offset < LONG_MIN)
        return AVERROR(EINVAL);

    if (fseek(file, (long)offset, origin) != 0)
        return AVERROR(errno ? errno : EIO);

    current = ftell(file);

    if (current < 0)
        return AVERROR(errno ? errno : EIO);

    return (int64_t)current;
}

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

static void display_frame(
    struct SwsContext *scaler,
    AVFrame *frame,
    uint8_t *rgb,
    int rgbStride)
{
    uint8_t *destData[4] = {
        rgb, NULL, NULL, NULL
    };

    int destStride[4] = {
        rgbStride, 0, 0, 0
    };

    sws_scale(
        scaler,
        (const uint8_t * const *)frame->data,
        frame->linesize,
        0,
        frame->height,
        destData,
        destStride
    );

    u16 width = 0;
    u16 height = 0;

    u8 *fb = gfxGetFramebuffer(
        GFX_TOP,
        GFX_LEFT,
        &width,
        &height
    );

    /*
     * Le framebuffer interne de l'écran supérieur est 240 x 400,
     * alors que l'image vidéo est 400 x 240.
     */
    if (!fb || width != FB_WIDTH || height != FB_HEIGHT) {
        gspWaitForVBlank();
        return;
    }

    /*
     * Le framebuffer de la 3DS est stocké de côté.
     * Le format par défaut est BGR8, pas RGB24.
     */
    for (int y = 0; y < SCREEN_H; y++) {
        for (int x = 0; x < SCREEN_W; x++) {
            const uint8_t *pixel =
                &rgb[y * rgbStride + x * 3];

            unsigned int pos =
                (unsigned int)x * FB_HEIGHT
                + (FB_HEIGHT - 1 - y);

            pos *= 3;

            fb[pos + 0] = pixel[2];
            fb[pos + 1] = pixel[1];
            fb[pos + 2] = pixel[0];
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

    /*
     * Attend que ce buffer ait fini d'être joué avant de le réutiliser.
     */
    while (aptMainLoop() &&
           buf->status == NDSP_WBUF_PLAYING) {
        gspWaitForVBlank();
    }

    if (!aptMainLoop())
        return;

    size_t bytes =
        (size_t)samples * AUDIO_CHANNELS * sizeof(int16_t);

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
    AVIOContext *customIO = NULL;

    AVCodecContext *videoCodec = NULL;
    AVCodecContext *audioCodec = NULL;

    AVPacket *packet = NULL;
    AVFrame *frame = NULL;

    SwsContext *scaler = NULL;
    SwrContext *resampler = NULL;

    FILE *videoFile = NULL;

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
            AUDIO_BUFFER_SAMPLES *
            AUDIO_CHANNELS *
            sizeof(int16_t)
        );

        if (!audioMemory[i]) {
            result = AVERROR(ENOMEM);
            goto error;
        }
    }

    if (ndspInit() != 0) {
        result = AVERROR_EXTERNAL;
        goto error;
    }

    ndspStarted = 1;

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
    ndspChnSetRate(0, AUDIO_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);

    /*
     * Ouverture du fichier AVI depuis la carte SD.
     */
    videoFile = fopen(VIDEO_PATH, "rb");

    if (!videoFile) {
        result = AVERROR(errno ? errno : ENOENT);
        goto error;
    }

    /*
     * Fournit à FFmpeg une entrée personnalisée pour éviter
     * l'erreur de protocole liée à sdmc:.
     */
    {
        uint8_t *ioBuffer = av_malloc(IO_BUFFER_SIZE);

        if (!ioBuffer) {
            result = AVERROR(ENOMEM);
            goto error;
        }

        customIO = avio_alloc_context(
            ioBuffer,
            IO_BUFFER_SIZE,
            0,
            videoFile,
            read_file_packet,
            NULL,
            seek_file_packet
        );

        if (!customIO) {
            av_free(ioBuffer);
            result = AVERROR(ENOMEM);
            goto error;
        }
    }

    format = avformat_alloc_context();

    if (!format) {
        result = AVERROR(ENOMEM);
        goto error;
    }

    format->pb = customIO;
    format->flags |= AVFMT_FLAG_CUSTOM_IO;

    result = avformat_open_input(
        &format,
        NULL,
        NULL,
        NULL
    );

    if (result < 0)
        goto error;

    result = avformat_find_stream_info(format, NULL);

    if (result < 0)
        goto error;

    /*
     * Recherche des flux vidéo et audio.
     */
    for (unsigned int i = 0; i < format->nb_streams; i++) {
        AVStream *stream = format->streams[i];

        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            videoStream < 0) {
            videoStream = (int)i;
        }

        if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
            audioStream < 0) {
            audioStream = (int)i;
        }
    }

    if (videoStream < 0) {
        result = AVERROR_STREAM_NOT_FOUND;
        goto error;
    }

    /*
     * Initialise le décodeur vidéo.
     */
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

        result = avcodec_parameters_to_context(
            videoCodec,
            params
        );

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
        NULL,
        NULL,
        NULL
    );

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

    /*
     * Initialise le décodeur audio si le fichier possède une piste audio.
     */
    if (audioStream >= 0) {
        AVCodecParameters *params =
            format->streams[audioStream]->codecpar;

        const AVCodec *decoder =
            avcodec_find_decoder(params->codec_id);

        if (decoder) {
            audioCodec = avcodec_alloc_context3(decoder);

            if (audioCodec) {
                result = avcodec_parameters_to_context(
                    audioCodec,
                    params
                );

                if (result >= 0) {
                    result = avcodec_open2(
                        audioCodec,
                        decoder,
                        NULL
                    );
                }

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
                            : 2
                    );
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
                    NULL
                );

                av_channel_layout_uninit(&inputLayout);
                av_channel_layout_uninit(&outputLayout);

                if (result < 0 ||
                    !resampler ||
                    swr_init(resampler) < 0) {
                    swr_free(&resampler);
                    avcodec_free_context(&audioCodec);
                    result = 0;
                }
            }
        }
    }

    /*
     * Lecture des paquets vidéo et audio.
     */
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
                           videoCodec,
                           frame) >= 0) {

                    display_frame(
                        scaler,
                        frame,
                        rgb,
                        SCREEN_W * 3
                    );

                    av_frame_unref(frame);
                }
            }
        } else if (audioCodec &&
                   resampler &&
                   packet->stream_index == audioStream) {

            result = avcodec_send_packet(audioCodec, packet);

            if (result >= 0) {
                while (avcodec_receive_frame(
                           audioCodec,
                           frame) >= 0) {

                    int capacity = (int)av_rescale_rnd(
                        swr_get_delay(
                            resampler,
                            audioCodec->sample_rate
                        ) + frame->nb_samples,
                        AUDIO_RATE,
                        audioCodec->sample_rate,
                        AV_ROUND_UP
                    );

                    if (capacity > 0 &&
                        capacity <= AUDIO_BUFFER_SAMPLES) {

                        audioConverted = malloc(
                            (size_t)capacity *
                            AUDIO_CHANNELS *
                            sizeof(int16_t)
                        );

                        if (audioConverted) {
                            uint8_t *out[] = {
                                audioConverted
                            };

                            int converted = swr_convert(
                                resampler,
                                out,
                                capacity,
                                (const uint8_t **)frame->extended_data,
                                frame->nb_samples
                            );

                            if (converted > 0) {
                                queue_audio(
                                    audioConverted,
                                    converted
                                );
                            }

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

    if (customIO) {
        av_freep(&customIO->buffer);
        avio_context_free(&customIO);
    }

    if (videoFile)
        fclose(videoFile);

    if (ndspStarted)
        ndspExit();

    for (int i = 0; i < AUDIO_BUFFER_COUNT; i++) {
        if (audioMemory[i])
            linearFree(audioMemory[i]);
    }

    gfxExit();

    return 0;
}