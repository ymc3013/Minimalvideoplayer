
#ifndef VIDEO_H
#define VIDEO_H

#include <libavutil/frame.h>
#include <libswscale/swscale.h>
#include <3ds.h>

typedef struct {
    struct SwsContext *scaler;
    int width;
    int height;
} VideoDecoder;

int video_init(VideoDecoder *video, int sourceWidth,
               int sourceHeight, enum AVPixelFormat sourceFormat);

void video_display(VideoDecoder *video, AVFrame *frame);

void video_close(VideoDecoder *video);

#endif
