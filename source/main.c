#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

#define VIDEO_PATH "sdmc:/Movies/test.avi"
#define SCREEN_W 400
#define SCREEN_H 240

static void draw_message(const char *message)
{
    consoleInit(GFX_TOP, NULL);
    printf("%s\n", message);
    printf("\n");
    printf("Minimalvideoplayer\n");
    printf("Video: %s\n", VIDEO_PATH);
}

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    romfsInit();

    ndspInit();
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);

    AVFormatContext *format = NULL;
    AVCodecContext *video_codec = NULL;
    AVCodecContext *audio_codec = NULL;
    AVFrame *frame = NULL;
    AVPacket *packet = NULL;

    int video_index = -1;
    int audio_index = -1;

    if (avformat_open_input(&format, VIDEO_PATH, NULL, NULL) < 0) {
        draw_message("Unable to open test.avi");
        goto cleanup;
    }

    if (avformat_find_stream_info(format, NULL) < 0) {
        draw_message("Unable to read AVI information");
        goto cleanup;
    }

    for (unsigned int i = 0; i < format->nb_streams; i++) {
        enum AVMediaType type =
            format->streams[i]->codecpar->codec_type;

        if (type == AVMEDIA_TYPE_VIDEO && video_index < 0)
            video_index = (int)i;

        if (type == AVMEDIA_TYPE_AUDIO && audio_index < 0)
            audio_index = (int)i;
    }

    if (video_index < 0) {
        draw_message("No video stream found");
        goto cleanup;
    }

    {
        AVStream *stream = format->streams[video_index];
        const AVCodec *decoder =
            avcodec_find_decoder(stream->codecpar->codec_id);

        if (!decoder) {
            draw_message("Video decoder not available");
            goto cleanup;
        }

        video_codec = avcodec_alloc_context3(decoder);

        if (!video_codec ||
            avcodec_parameters_to_context(
                video_codec, stream->codecpar) < 0 ||
            avcodec_open2(video_codec, decoder, NULL) < 0) {
            draw_message("Unable to initialize video decoder");
            goto cleanup;
        }
    }

    if (audio_index >= 0) {
        AVStream *stream = format->streams[audio_index];
        const AVCodec *decoder =
            avcodec_find_decoder(stream->codecpar->codec_id);

        if (decoder) {
            audio_codec = avcodec_alloc_context3(decoder);

            if (!audio_codec ||
                avcodec_parameters_to_context(
                    audio_codec, stream->codecpar) < 0 ||
                avcodec_open2(audio_codec, decoder, NULL) < 0) {
                avcodec_free_context(&audio_codec);
                audio_codec = NULL;
            }
        }
    }

    frame = av_frame_alloc();
    packet = av_packet_alloc();

    if (!frame || !packet) {
        draw_message("Not enough memory");
        goto cleanup;
    }

    struct SwsContext *scale = sws_getContext(
        video_codec->width,
        video_codec->height,
        video_codec->pix_fmt,
        SCREEN_W,
        SCREEN_H,
        AV_PIX_FMT_RGB565LE,
        SWS_BILINEAR,
        NULL, NULL, NULL
    );

    if (!scale) {
        draw_message("Unable to initialize video scaling");
        goto cleanup;
    }

    uint8_t *pixels = malloc(SCREEN_W * SCREEN_H * 2);

    if (!pixels) {
        sws_freeContext(scale);
        draw_message("Not enough memory for video");
        goto cleanup;
    }

    uint8_t *dst_data[4] = { pixels, NULL, NULL, NULL };
    int dst_linesize[4] = { SCREEN_W * 2, 0, 0, 0 };

    consoleInit(GFX_BOTTOM, NULL);
    printf("Playing: test.avi\n");
    printf("Press START to exit.\n");

    while (aptMainLoop()) {
        hidScanInput();

        if (hidKeysDown() & KEY_START)
            break;

        if (av_read_frame(format, packet) < 0)
            break;

        if (packet->stream_index == video_index) {
            if (avcodec_send_packet(video_codec, packet) >= 0) {
                while (avcodec_receive_frame(video_codec, frame) >= 0) {
                    sws_scale(
                        scale,
                        (const uint8_t * const *)frame->data,
                        frame->linesize,
                        0,
                        video_codec->height,
                        dst_data,
                        dst_linesize
                    );

                    u16 *screen = (u16 *)gfxGetFramebuffer(
                        GFX_TOP, GFX_LEFT, NULL, NULL
                    );

                    if (screen) {
                        for (int y = 0; y < SCREEN_H; y++) {
                            memcpy(
                                &screen[y * SCREEN_W],
                                &pixels[y * SCREEN_W],
                                SCREEN_W * sizeof(u16)
                            );
                        }

                        gfxFlushBuffers();
                        gfxSwapBuffers();
                    }

                    gspWaitForVBlank();
                }
            }
        }

        /*
         * Audio decoding and playback are not implemented in this
         * initial source. A working audio pipeline needs compatible
         * FFmpeg libraries and NDSP buffer management.
         */
        av_packet_unref(packet);
    }

    free(pixels);
    sws_freeContext(scale);

cleanup:
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&video_codec);
    avcodec_free_context(&audio_codec);
    avformat_close_input(&format);

    ndspExit();
    romfsExit();
    gfxExit();

    return 0;
}