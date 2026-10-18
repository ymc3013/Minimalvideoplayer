
#include "video.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int video_init(
    VideoDecoder *video,
    int sourceWidth,
    int sourceHeight,
    enum AVPixelFormat sourceFormat
)
{
    if (!video || sourceWidth <= 0 || sourceHeight <= 0)
        return -1;

    video->scaler = NULL;
    video->width = 400;
    video->height = 240;

    video->scaler = sws_getContext(
        sourceWidth,
        sourceHeight,
        sourceFormat,
        video->width,
        video->height,
        AV_PIX_FMT_BGR24,
        SWS_BILINEAR,
        NULL,
        NULL,
        NULL
    );

    return video->scaler ? 0 : -1;
}

void video_display(VideoDecoder *video, AVFrame *frame)
{
    if (!video || !frame ||
        frame->width <= 0 || frame->height <= 0)
        return;

    int width = video->width;
    int height = video->height;
    int stride = width * 3;

    if (width != 400 || height != 240)
        return;

    /*
     * Recrée le convertisseur si le format de l'image change.
     */
    video->scaler = sws_getCachedContext(
        video->scaler,
        frame->width,
        frame->height,
        (enum AVPixelFormat)frame->format,
        width,
        height,
        AV_PIX_FMT_BGR24,
        SWS_BILINEAR,
        NULL,
        NULL,
        NULL
    );

    if (!video->scaler)
        return;

    uint8_t *pixels = malloc((size_t)stride * height);

    if (!pixels)
        return;

    uint8_t *dstData[4] = { pixels, NULL, NULL, NULL };
    int dstStride[4] = { stride, 0, 0, 0 };

    int rows = sws_scale(
        video->scaler,
        (const uint8_t * const *)frame->data,
        frame->linesize,
        0,
        frame->height,
        dstData,
        dstStride
    );

    if (rows != height) {
        free(pixels);
        return;
    }

    u16 fbWidth = 0;
    u16 fbHeight = 0;

    u8 *fb = gfxGetFramebuffer(
        GFX_TOP,
        GFX_LEFT,
        &fbWidth,
        &fbHeight
    );

    /*
     * Sur l'écran supérieur, le framebuffer libctru
     * est généralement rapporté comme 240 x 400.
     */
    if (!fb || fbWidth != 240 || fbHeight != 400) {
        free(pixels);
        return;
    }

    /*
     * Le framebuffer 3DS utilise le format BGR8.
     * L'image source est en 400 x 240 ; l'accès mémoire
     * au framebuffer utilise une disposition en colonnes.
     */
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const uint8_t *src =
                &pixels[(size_t)y * stride + (size_t)x * 3];

            size_t pos =
                ((size_t)x * 240 + (239 - y)) * 3;

            fb[pos]     = src[0];
            fb[pos + 1] = src[1];
            fb[pos + 2] = src[2];
        }
    }

    free(pixels);

    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

void video_close(VideoDecoder *video)
{
    if (!video)
        return;

    sws_freeContext(video->scaler);
    video->scaler = NULL;
    video->width = 0;
    video->height = 0;
}
