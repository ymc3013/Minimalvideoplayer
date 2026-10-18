#include "avi.h"

#include <stdio.h>

int avi_open(AviFile *avi, const char *path)
{
    if (avi == NULL || path == NULL) {
        return -1;
    }

    avi->format = NULL;
    avi->videoStream = -1;
    avi->audioStream = -1;

    if (avformat_open_input(&avi->format, path, NULL, NULL) < 0) {
        fprintf(stderr, "AVI: impossible d'ouvrir le fichier.\n");
        return -1;
    }

    if (avformat_find_stream_info(avi->format, NULL) < 0) {
        fprintf(stderr, "AVI: impossible de lire les informations du fichier.\n");
        avformat_close_input(&avi->format);
        return -1;
    }

    for (unsigned int i = 0; i < avi->format->nb_streams; i++) {
        AVStream *stream = avi->format->streams[i];
        enum AVMediaType type = stream->codecpar->codec_type;

        if (type == AVMEDIA_TYPE_VIDEO && avi->videoStream < 0) {
            avi->videoStream = (int)i;
        } else if (type == AVMEDIA_TYPE_AUDIO && avi->audioStream < 0) {
            avi->audioStream = (int)i;
        }
    }

    if (avi->videoStream < 0) {
        fprintf(stderr, "AVI: aucune piste vidéo trouvée.\n");
        avformat_close_input(&avi->format);
        avi->audioStream = -1;
        return -1;
    }

    return 0;
}

int avi_read_packet(AviFile *avi, AVPacket *packet)
{
    if (avi == NULL || avi->format == NULL || packet == NULL) {
        return -1;
    }

    return av_read_frame(avi->format, packet);
}

void avi_close(AviFile *avi)
{
    if (avi == NULL) {
        return;
    }

    if (avi->format != NULL) {
        avformat_close_input(&avi->format);
    }

    avi->videoStream = -1;
    avi->audioStream = -1;
}