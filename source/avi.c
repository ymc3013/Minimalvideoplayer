
#include "avi.h"

#include <string.h>

int avi_open(AviFile *avi, const char *path)
{
    if (!avi || !path)
        return AVERROR(EINVAL);

    memset(avi, 0, sizeof(*avi));
    avi->videoStream = -1;
    avi->audioStream = -1;

    int ret = avformat_open_input(&avi->format, path, NULL, NULL);
    if (ret < 0)
        return ret;

    ret = avformat_find_stream_info(avi->format, NULL);
    if (ret < 0) {
        avi_close(avi);
        return ret;
    }

    for (unsigned int i = 0; i < avi->format->nb_streams; i++) {
        AVMediaType type = avi->format->streams[i]->codecpar->codec_type;

        if (type == AVMEDIA_TYPE_VIDEO && avi->videoStream < 0)
            avi->videoStream = (int)i;

        if (type == AVMEDIA_TYPE_AUDIO && avi->audioStream < 0)
            avi->audioStream = (int)i;
    }

    if (avi->videoStream < 0) {
        avi_close(avi);
        return AVERROR_STREAM_NOT_FOUND;
    }

    return 0;
}

int avi_read_packet(AviFile *avi, AVPacket *packet)
{
    if (!avi || !avi->format || !packet)
        return AVERROR(EINVAL);

    return av_read_frame(avi->format, packet);
}

void avi_close(AviFile *avi)
{
    if (!avi)
        return;

    avformat_close_input(&avi->format);
    avi->videoStream = -1;
    avi->audioStream = -1;
}
