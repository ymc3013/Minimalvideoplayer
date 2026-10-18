
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
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/time.h>
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
#define PRELOAD_AUDIO_BUFFERS 2

static ndspWaveBuf audioBuffers[AUDIO_BUFFER_COUNT];
static int16_t *audioMemory[AUDIO_BUFFER_COUNT];
static int audioIndex = 0;
static int ndspStarted = 0;

static int64_t playbackStartUs = 0;
static int64_t firstVideoPts = AV_NOPTS_VALUE;
static int playbackClockStarted = 0;

/* ---------------------------------------------------------
   Lecture du fichier depuis la carte SD
   --------------------------------------------------------- */

static int read_file_packet(
    void *opaque,
    uint8_t *buffer,
    int buffer_size)
{
    FILE *file = (FILE *)opaque;

    if (buffer_size <= 0)
        return AVERROR(EINVAL);

    size_t count = fread(
        buffer,
        1,
        (size_t)buffer_size,
        file
    );

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

/* ---------------------------------------------------------
   Messages d'erreur
   --------------------------------------------------------- */

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

/* ---------------------------------------------------------
   Synchronisation des images vidéo
   --------------------------------------------------------- */

static void wait_until_us(int64_t targetUs)
{
    while (aptMainLoop()) {
        int64_t now = av_gettime_relative();
        int64_t remaining = targetUs - now;

        if (remaining <= 0)
            break;

        /*
         * Sommeil court pour laisser du temps au processeur.
         * svcSleepThread utilise des nanosecondes.
         */
        if (remaining > 3000)
            svcSleepThread(1000000);
        else
            svcSleepThread(remaining * 1000);
    }
}

static void pace_video_frame(
    AVFrame *frame,
    AVStream *stream)
{
    int64_t now = av_gettime_relative();

    if (!playbackClockStarted) {
        playbackClockStarted = 1;
        playbackStartUs = now;

        if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
            firstVideoPts = frame->best_effort_timestamp;

        return;
    }

    if (frame->best_effort_timestamp != AV_NOPTS_VALUE &&
        firstVideoPts != AV_NOPTS_VALUE) {

        int64_t frameOffsetUs = av_rescale_q(
            frame->best_effort_timestamp - firstVideoPts,
            stream->time_base,
            AV_TIME_BASE_Q
        );

        wait_until_us(playbackStartUs + frameOffsetUs);
    }
}

/* ---------------------------------------------------------
   Affichage vidéo
   --------------------------------------------------------- */

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

    int rows = sws_scale(
        scaler,
        (const uint8_t * const *)frame->data,
        frame->linesize,
        0,
        frame->height,
        destData,
        destStride
    );

    if (rows <= 0)
        return;

    u16 width = 0;
    u16 height = 0;

    u8 *fb = gfxGetFramebuffer(
        GFX_TOP,
        GFX_LEFT,
        &width,
        &height
    );

    if (!fb || width != FB_WIDTH || height != FB_HEIGHT)
        return;

    /*
     * L'image convertie fait 400 x 240.
     * Le framebuffer est organisé en 240 x 400.
     *
     * Chaque pixel est écrit en BGR.
     */
    for (int y = 0; y < SCREEN_H; y++) {
        for (int x = 0; x < SCREEN_W; x++) {
            const uint8_t *pixel =
                &rgb[y * rgbStride + x * 3];

            unsigned int pos =
                (unsigned int)x * FB_WIDTH
                + (FB_WIDTH - 1 - y);

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

/* ---------------------------------------------------------
   Audio : buffers NDSP
   --------------------------------------------------------- */

static int queue_audio(
    const int16_t *data,
    int samples)
{
    if (!data || samples <= 0)
        return 0;

    int offset = 0;

    /*
     * Découpe les données en blocs de taille sûre.
     */
    while (offset < samples && aptMainLoop()) {
        int count = samples - offset;

        if (count > AUDIO_BUFFER_SAMPLES)
            count = AUDIO_BUFFER_SAMPLES;

        ndspWaveBuf *buf = &audioBuffers[audioIndex];

        /*
         * Ne jamais écraser un buffer encore utilisé par NDSP.
         */
        while (aptMainLoop() &&
               (buf->status == NDSP_WBUF_QUEUED ||
                buf->status == NDSP_WBUF_PLAYING)) {
            gspWaitForVBlank();
        }

        if (!aptMainLoop())
            return -1;

        size_t bytes =
            (size_t)count *
            AUDIO_CHANNELS *
            sizeof(int16_t);

        memcpy(
            audioMemory[audioIndex],
            data + (size_t)offset * AUDIO_CHANNELS,
            bytes
        );

        memset(buf, 0, sizeof(*buf));

        buf->data_vaddr = audioMemory[audioIndex];
        buf->nsamples = count;
        buf->looping = false;

        DSP_FlushDataCache(
            audioMemory[audioIndex],
            bytes
        );

        ndspChnWaveBufAdd(0, buf);

        audioIndex = (audioIndex + 1) % AUDIO_BUFFER_COUNT;
        offset += count;
    }

    return 0;
}

/* ---------------------------------------------------------
   Préparation et lecture
   --------------------------------------------------------- */

int main(void)
{
    AVFormatContext *format = NULL;
    AVIOContext *customIO = NULL;

    AVCodecContext *videoCodec = NULL;
    AVCodecContext *audioCodec = NULL;

    AVPacket *packet = NULL;
    AVFrame *videoFrame = NULL;
    AVFrame *audioFrame = NULL;

    SwsContext *scaler = NULL;
    SwrContext *resampler = NULL;

    FILE *videoFile = NULL;

    uint8_t *rgb = NULL;
    uint8_t *audioConverted = NULL;

    int videoStream = -1;
    int audioStream = -1;
    int result = 0;

    /*
     * Initialisation de l'écran et affichage du chargement.
     */
    gfxInitDefault();
    gfxSet3D(false);

    consoleInit(GFX_TOP, NULL);
    printf("Minimal Video Player\n\n");
    printf("Chargement de la video...\n");
    printf("Ouverture du fichier AVI...\n");
    printf("Veuillez patienter.");

    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();

    /*
     * Allocation des buffers audio.
     */
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

        memset(
            audioMemory[i],
            0,
            AUDIO_BUFFER_SAMPLES *
            AUDIO_CHANNELS *
            sizeof(int16_t)
        );

        memset(&audioBuffers[i], 0, sizeof(ndspWaveBuf));
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
     * Ouvre le fichier sans demander à FFmpeg de traiter
     * "sdmc:" comme un protocole réseau.
     */
    videoFile = fopen(VIDEO_PATH, "rb");

    if (!videoFile) {
        result = AVERROR(errno ? errno : ENOENT);
        goto error;
    }

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

    /*
     * FFmpeg analyse les pistes avant le début de la lecture.
     */
    result = avformat_find_stream_info(format, NULL);

    if (result < 0)
        goto error;

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

    /*
     * Conversion de la résolution source vers 400 x 240.
     */
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

    videoFrame = av_frame_alloc();
    audioFrame = av_frame_alloc();
    packet = av_packet_alloc();

    if (!videoFrame || !audioFrame || !packet) {
        result = AVERROR(ENOMEM);
        goto error;
    }

    /*
     * Initialise le décodeur audio si une piste existe.
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
     * L'initialisation est terminée : la lecture commence
     * uniquement après la préparation des décodeurs.
     */
    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();

    /*
     * Boucle principale.
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
                           videoFrame) >= 0) {

                    pace_video_frame(
                        videoFrame,
                        format->streams[videoStream]
                    );

                    display_frame(
                        scaler,
                        videoFrame,
                        rgb,
                        SCREEN_W * 3
                    );

                    av_frame_unref(videoFrame);
                }
            }
        }
        else if (audioCodec &&
                 resampler &&
                 packet->stream_index == audioStream) {

            result = avcodec_send_packet(audioCodec, packet);

            if (result >= 0) {
                while (avcodec_receive_frame(
                           audioCodec,
                           audioFrame) >= 0) {

                    int capacity = (int)av_rescale_rnd(
                        swr_get_delay(
                            resampler,
                            audioCodec->sample_rate
                        ) + audioFrame->nb_samples,
                        AUDIO_RATE,
                        audioCodec->sample_rate,
                        AV_ROUND_UP
                    );

                    if (capacity > 0) {
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
                                (const uint8_t **)
                                    audioFrame->extended_data,
                                audioFrame->nb_samples
                            );

                            if (converted > 0) {
                                queue_audio(
                                    (const int16_t *)audioConverted,
                                    converted
                                );
                            }

                            free(audioConverted);
                            audioConverted = NULL;
                        }
                    }

                    av_frame_unref(audioFrame);
                }
            }
        }

        av_packet_unref(packet);
    }

    /*
     * Attend la fin du son déjà envoyé à NDSP avant de quitter.
     */
    if (ndspStarted) {
        int pending;

        do {
            pending = 0;

            for (int i = 0; i < AUDIO_BUFFER_COUNT; i++) {
                if (audioBuffers[i].status == NDSP_WBUF_QUEUED ||
                    audioBuffers[i].status == NDSP_WBUF_PLAYING) {
                    pending = 1;
                    break;
                }
            }

            if (pending && aptMainLoop())
                gspWaitForVBlank();

        } while (pending && aptMainLoop());
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
    av_frame_free(&videoFrame);
    av_frame_free(&audioFrame);

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

        audioMemory[i] = NULL;
    }

    gfxExit();

    return 0;
}
