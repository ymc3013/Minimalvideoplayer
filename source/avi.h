
#ifndef AVI_H
#define AVI_H

#include <libavformat/avformat.h>

typedef struct {
    AVFormatContext *format;
    int videoStream;
    int audioStream;
} AviFile;

int avi_open(AviFile *avi, const char *path);

int avi_read_packet(AviFile *avi, AVPacket *packet);

void avi_close(AviFile *avi);

#endif
