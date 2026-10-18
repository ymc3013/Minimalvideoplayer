#include "avi.h"

#include <stdio.h>
#include <string.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

int avi_open(AVIContext *avi, const char *filename)
{
    if (avi == NULL || filename == NULL) {
        return -1;
    }

    memset(avi, 0, sizeof(*avi));
    avi->video_stream = -1;
    avi->audio_stream = -1;

    if (avformat_open_input(&avi->format, filename, NULL, NULL) < 0) {
        fprintf(stderr, "AVI: impossible d'ouvrir %s\n", filename);
        return -1;
    }

    if (avformat_find_stream_info(avi->format, NULL) < 0) {
        fprintf(stderr, "AVI: informations de flux introuvables\n");
        avformat_close_input(&avi->format);
        return -1;
    }

    for (unsigned int i = 0; i < avi->format->nb_streams; i++) {
        AVStream *stream = avi->format->streams[i];
        enum AVMediaType type = stream->codecpar->codec_type;

        if (type == AVMEDIA_TYPE_VIDEO && avi->video_stream < 0) {
            avi->video_stream = (int)i;
        } else if (type == AVMEDIA_TYPE_AUDIO && avi->audio_stream < 0) {
            avi->audio_stream = (int)i;
        }
    }

    if (avi->video_stream < 0) {
        fprintf(stderr, "AVI: aucune piste vidéo trouvée\n");
        avformat_close_input(&avi->format);
        avi->audio_stream = -1;
        return -1;
    }

    return 0;
}

void avi_close(AVIContext *avi)
{
    if (avi == NULL) {
        return;
    }

    if (avi->format != NULL) {
        avformat_close_input(&avi->format);
    }

    avi->video_stream = -1;
    avi->audio_stream = -1;
}