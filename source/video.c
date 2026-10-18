
#include "video.h"

#include <stdlib.h>
#include <stdint.h>
#include <libavutil/pixfmt.h>

int video_init(VideoDecoder *video, int sourceWidth,
               int sourceHeight, enum AVPixelFormat sourceFormat)
{
    if (!video)
        return -1;

    video->width = 400;
    video->height = 240;

    video->scaler = sws_getContext(
        sourceWidth, sourceHeight, sourceFormat,
        video->width, video->height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, NULL, NULL, NULL);

    return video->scaler ? 0 : -1;
}

void video_display(VideoDecoder *video, AVFrame *frame)
{
    if (!video || !video->scaler || !frame)
        return;

    const int stride = video->width * 3;
    uint8_t *rgb = malloc((size_t)stride * video->height);

    if (!rgb)
        return;

    uint8_t *dst[4] = { rgb, NULL, NULL, NULL };
    int dstStride[4] = { stride, 0, 0, 0 };

    sws_scale(video->scaler,
              (const uint8_t * const *)frame->data,
              frame->linesize, 0, frame->height,
              dst, dstStride);

    u16 screenWidth, screenHeight;
    u8 *fb = gfxGetFramebuffer(
        GFX_TOP, GFX_LEFT, &screenWidth, &screenHeight);

    if (fb && screenWidth == 400 && screenHeight == 240) {
        for (int y = 0; y < 240; y++) {
            for (int x = 0; x < 400; x++) {
                uint8_t *src = &rgb[y * stride + x * 3];

                size_t pos = ((size_t)(239 - y) * 400 + x) * 3;
                fb[pos] = src[0];
                fb[pos + 1] = src[1];
                fb[pos + 2] = src[2];
            }
        }

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    free(rgb);
}

void video_close(VideoDecoder *video)
{
    if (!video)
        return;

    sws_freeContext(video->scaler);
    video->scaler = NULL;
}
