/*
 * Minimal AVI player - reference implementation for Nintendo 3DS (devkitARM + libctru + FFmpeg)
 *
 * STATUS: NOT COMPILED, NOT TESTED (written without access to devkitARM or your FFmpeg build).
 *
 * Design points (codec-agnostic):
 *  - Video: sws_scale to BGR24 using the REAL frame->format / width / height and the REAL
 *    destination linesize, then rotated copy into the 3DS framebuffer (240x400, column-major, BGR8).
 *  - Audio: swresample is created lazily from the first DECODED frame (real sample_fmt, rate,
 *    channel layout), converted to interleaved S16 stereo at the SAME sample rate.
 *    NDSP rate/format are set from those real values. nsamples is in samples, not bytes.
 *  - Sync: audio is the master clock (NDSP sample position). Video frames wait until due,
 *    and are skipped (not rendered) when more than 150 ms late.
 *  - Fallback: wall clock if there is no audio or NDSP is unavailable (missing dspfirm.cdc).
 *
 * Controls: A = pause/resume, START = quit.
 * File: first *.avi found in sdmc:/videos/ then sdmc:/ then sdmc:/3ds/
 */

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <stdbool.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/mathematics.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavformat/avio.h>
#include <stdarg.h>
#include <errno.h>

#define SCR_W 400
#define SCR_H 240
#define FB_W  240            /* framebuffer "width" = physical screen height */

#define ACH          0
#define ABUFS        8
#define ABUF_SAMPLES 16384   /* max samples per NDSP buffer (stereo S16 => 4 bytes/sample) */
#define LATE_DROP    0.150   /* seconds */

#define HAS_CHLAYOUT (LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 24, 100))

typedef struct {
    ndspWaveBuf wb[ABUFS];
    int16_t    *mem;
    int         next;
    int64_t     start[ABUFS];   /* absolute sample index at which each buffer starts */
    int64_t     queued;         /* total samples queued so far */
    double      last_clock;     /* monotonic, seconds of audio played */
    int         rate;
} Audio;

typedef struct {
    AVFormatContext *fmt;
    AVCodecContext  *vctx, *actx;
    AVStream        *vst, *ast;
    AVFrame         *vframe, *aframe;
    AVPacket        *pkt;

    /* video scaling */
    struct SwsContext *sws;
    int       sws_w, sws_h, sws_fmt;
    uint8_t  *dst_data[4];
    int       dst_linesize[4];
    int       dw, dh;           /* destination size on screen */
    bool      dst_alloc;
    double    frame_dur;
    double    next_vpts;

    /* audio */
    struct SwrContext *swr;
    Audio     au;
    int16_t  *tmp;              /* converted samples */
    int       tmp_cap;          /* in samples (per channel) */
    bool      has_audio;
    bool      ndsp_ok;
    bool      aud_ready;
    double    audio_base;       /* pts (s) of first audio frame */

    /* fallback wall clock */
    u64       wall0;
    u64       paused_ms;
    double    vbase_pts;
    bool      have_vbase;

    bool      paused;
    bool      quit;
} Ctx;

/* ------------------------------------------------------------------ logging / diagnostics */

static FILE *g_log;   /* sdmc:/3ds/minivideo_log.txt */

static void say(const char *f, ...)
{
    char b[512];
    va_list v;
    va_start(v, f);
    vsnprintf(b, sizeof(b), f, v);
    va_end(v);
    fputs(b, stdout);
    if (g_log) { fputs(b, g_log); fflush(g_log); }
}

/* FFmpeg's own messages: INFO and below go to the log file, WARNING and below also to the screen */
static void av_log_cb(void *ptr, int level, const char *fmt, va_list vl)
{
    (void)ptr;
    if (level > AV_LOG_INFO) return;
    char line[512];
    vsnprintf(line, sizeof(line), fmt, vl);
    if (g_log) { fputs(line, g_log); fflush(g_log); }
    if (level <= AV_LOG_WARNING) fputs(line, stdout);
}

static void check_ffmpeg_build(void)
{
    unsigned rf = avformat_version(), rc = avcodec_version(), ru = avutil_version();
    unsigned rs = swscale_version(), rr = swresample_version();
    say("FFmpeg runtime: avformat %u.%u avcodec %u.%u avutil %u.%u\n",
        rf >> 16, (rf >> 8) & 255, rc >> 16, (rc >> 8) & 255, ru >> 16, (ru >> 8) & 255);

#define CHK_VER(name, rt, hdr) \
    if (((rt) >> 8) != ((unsigned)(hdr) >> 8)) \
        say("WARNING %s: headers %u.%u != library %u.%u (struct layouts may differ!)\n", name, \
            (unsigned)(hdr) >> 16, ((unsigned)(hdr) >> 8) & 255, (rt) >> 16, ((rt) >> 8) & 255)
    CHK_VER("avformat",   rf, LIBAVFORMAT_VERSION_INT);
    CHK_VER("avcodec",    rc, LIBAVCODEC_VERSION_INT);
    CHK_VER("avutil",     ru, LIBAVUTIL_VERSION_INT);
    CHK_VER("swscale",    rs, LIBSWSCALE_VERSION_INT);
    CHK_VER("swresample", rr, LIBSWRESAMPLE_VERSION_INT);
#undef CHK_VER

    const AVInputFormat *avi = av_find_input_format("avi");
    say("demuxer 'avi': %s\n", avi ? "present" : "MISSING from this FFmpeg build");

    bool has_file = false;
    void *op = NULL;
    const char *pn;
    while ((pn = avio_enum_protocols(&op, 0))) if (!strcmp(pn, "file")) has_file = true;
    say("protocol 'file': %s\n", has_file ? "present" : "MISSING from this FFmpeg build");

    if (g_log) fprintf(g_log, "FFmpeg configure line: %s\n", avformat_configuration());
}

static const char *explain_err(int r)
{
    switch (r) {
    case AVERROR_PROTOCOL_NOT_FOUND: return "protocol not found (URL scheme unknown or 'file' protocol not built in)";
    case AVERROR_DEMUXER_NOT_FOUND:  return "no demuxer for this data (not built in?)";
    case AVERROR_INVALIDDATA:        return "data not recognised / corrupt (not a valid AVI, or truncated)";
    case AVERROR(ENOENT):            return "file not found";
    case AVERROR(EIO):               return "I/O error while reading";
    case AVERROR(ENOMEM):            return "out of memory";
    case AVERROR(EACCES):            return "permission denied";
    default:                         return "see message above";
    }
}

/* ------------------------------------------------------------------ input / pause */

static bool poll_input(Ctx *c)
{
    if (!aptMainLoop()) { c->quit = true; return false; }
    hidScanInput();
    u32 kd = hidKeysDown();
    if (kd & KEY_START) { c->quit = true; return false; }
    if (kd & KEY_A) {
        c->paused = !c->paused;
        if (c->paused) {
            if (c->ndsp_ok && c->aud_ready) ndspChnSetPaused(ACH, true);
            u64 t0 = osGetTime();
            while (c->paused) {
                if (!aptMainLoop()) { c->quit = true; return false; }
                hidScanInput();
                u32 k = hidKeysDown();
                if (k & KEY_START) { c->quit = true; return false; }
                if (k & KEY_A) c->paused = false;
                gspWaitForVBlank();
            }
            c->paused_ms += osGetTime() - t0;
            if (c->ndsp_ok && c->aud_ready) ndspChnSetPaused(ACH, false);
        }
    }
    return true;
}

/* ------------------------------------------------------------------ audio */

static void audio_init_buffers(Audio *a)
{
    memset(a, 0, sizeof(*a));
    a->mem = (int16_t *)linearAlloc((size_t)ABUFS * ABUF_SAMPLES * 2 * sizeof(int16_t));
    for (int i = 0; i < ABUFS; i++) {
        a->wb[i].data_vaddr = a->mem + (size_t)i * ABUF_SAMPLES * 2;
        a->wb[i].status = NDSP_WBUF_FREE;
    }
}

static double audio_clock(Audio *a)
{
    for (int i = 0; i < ABUFS; i++) {
        if (a->wb[i].status == NDSP_WBUF_PLAYING) {
            u32 pos = ndspChnGetSamplePos(ACH);
            if (pos > a->wb[i].nsamples) pos = a->wb[i].nsamples;
            double t = (double)(a->start[i] + pos) / (double)a->rate;
            if (t > a->last_clock) a->last_clock = t;
            break;
        }
    }
    return a->last_clock;
}

/* queue n interleaved stereo S16 samples; blocks (with timeout) if all buffers are busy */
static bool audio_push(Ctx *c, const int16_t *src, int n)
{
    Audio *a = &c->au;
    while (n > 0) {
        ndspWaveBuf *wb = &a->wb[a->next];
        int waited = 0;
        while (wb->status == NDSP_WBUF_QUEUED || wb->status == NDSP_WBUF_PLAYING) {
            svcSleepThread(1000000LL);                 /* 1 ms */
            if (++waited > 2000) return false;         /* 2 s: give up */
            if (!poll_input(c)) return false;
        }
        int chunk = n > ABUF_SAMPLES ? ABUF_SAMPLES : n;
        int16_t *dst = (int16_t *)wb->data_vaddr;
        memcpy(dst, src, (size_t)chunk * 2 * sizeof(int16_t));
        DSP_FlushDataCache(dst, (u32)chunk * 2 * sizeof(int16_t));
        wb->nsamples = (u32)chunk;                      /* SAMPLES, not bytes */
        wb->looping  = false;
        a->start[a->next] = a->queued;
        a->queued += chunk;
        ndspChnWaveBufAdd(ACH, wb);
        a->next = (a->next + 1) % ABUFS;
        src += (size_t)chunk * 2;
        n   -= chunk;
    }
    return true;
}

/* create swr + configure NDSP from the first REAL decoded frame */
static bool audio_setup_from_frame(Ctx *c, const AVFrame *f)
{
    int rate = f->sample_rate > 0 ? f->sample_rate : c->actx->sample_rate;
    if (rate <= 0) return false;

#if HAS_CHLAYOUT
    AVChannelLayout in_l = {0}, out_l = {0};
    int nb = f->ch_layout.nb_channels > 0 ? f->ch_layout.nb_channels : c->actx->ch_layout.nb_channels;
    if (f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || f->ch_layout.nb_channels == 0)
        av_channel_layout_default(&in_l, nb > 0 ? nb : 2);
    else
        av_channel_layout_copy(&in_l, &f->ch_layout);
    av_channel_layout_default(&out_l, 2);
    c->swr = NULL;
    if (swr_alloc_set_opts2(&c->swr, &out_l, AV_SAMPLE_FMT_S16, rate,
                            &in_l, (enum AVSampleFormat)f->format, rate, 0, NULL) < 0) {
        av_channel_layout_uninit(&in_l); av_channel_layout_uninit(&out_l);
        return false;
    }
    av_channel_layout_uninit(&in_l); av_channel_layout_uninit(&out_l);
#else
    int nb = f->channels > 0 ? f->channels : c->actx->channels;
    int64_t in_l = f->channel_layout ? (int64_t)f->channel_layout : av_get_default_channel_layout(nb);
    c->swr = swr_alloc_set_opts(NULL, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, rate,
                                in_l, (enum AVSampleFormat)f->format, rate, 0, NULL);
    if (!c->swr) return false;
#endif
    if (swr_init(c->swr) < 0) { swr_free(&c->swr); return false; }

    c->au.rate = rate;
    if (c->ndsp_ok) {
        float mix[12] = {0};
        mix[0] = mix[1] = 1.0f;
        ndspChnReset(ACH);
        ndspChnSetInterp(ACH, NDSP_INTERP_LINEAR);
        ndspChnSetRate(ACH, (float)rate);
        ndspChnSetFormat(ACH, NDSP_FORMAT_STEREO_PCM16);
        ndspChnSetMix(ACH, mix);
    }
    printf("Audio: %s %s %d Hz, %d ch -> S16 stereo\n",
           avcodec_get_name(c->actx->codec_id),
           av_get_sample_fmt_name((enum AVSampleFormat)f->format), rate, nb);
    return true;
}

static void handle_audio(Ctx *c, AVPacket *pkt)
{
    if (!c->actx) return;
    if (avcodec_send_packet(c->actx, pkt) < 0) return;
    while (avcodec_receive_frame(c->actx, c->aframe) >= 0) {
        AVFrame *f = c->aframe;
        if (!c->swr) {
            if (!audio_setup_from_frame(c, f)) { c->has_audio = false; av_frame_unref(f); return; }
            int64_t t = f->best_effort_timestamp;
            c->audio_base = (t != AV_NOPTS_VALUE) ? t * av_q2d(c->ast->time_base) : 0.0;
        }
        int out_cap = swr_get_out_samples(c->swr, f->nb_samples);
        if (out_cap < f->nb_samples) out_cap = f->nb_samples;
        out_cap += 256;
        if (out_cap > c->tmp_cap) {
            free(c->tmp);
            c->tmp = (int16_t *)malloc((size_t)out_cap * 2 * sizeof(int16_t));
            c->tmp_cap = c->tmp ? out_cap : 0;
            if (!c->tmp) { av_frame_unref(f); return; }
        }
        uint8_t *outp[1] = { (uint8_t *)c->tmp };
        int n = swr_convert(c->swr, outp, c->tmp_cap,
                            (const uint8_t **)f->extended_data, f->nb_samples);
        av_frame_unref(f);
        if (n <= 0) continue;
        if (c->ndsp_ok) {
            if (!audio_push(c, c->tmp, n)) { c->quit = c->quit || !aptMainLoop(); return; }
            c->aud_ready = true;
        }
    }
}

/* ------------------------------------------------------------------ video */

static bool video_setup_scaler(Ctx *c, const AVFrame *f)
{
    if (c->sws && c->sws_w == f->width && c->sws_h == f->height && c->sws_fmt == f->format)
        return true;

    if (c->sws) { sws_freeContext(c->sws); c->sws = NULL; }
    if (c->dst_alloc) { av_freep(&c->dst_data[0]); c->dst_alloc = false; }

    double s = (double)SCR_W / f->width;
    double sy = (double)SCR_H / f->height;
    if (sy < s) s = sy;
    c->dw = ((int)(f->width * s)) & ~1;
    c->dh = ((int)(f->height * s)) & ~1;
    if (c->dw < 2) c->dw = 2;
    if (c->dh < 2) c->dh = 2;

    c->sws = sws_getContext(f->width, f->height, (enum AVPixelFormat)f->format,
                            c->dw, c->dh, AV_PIX_FMT_BGR24,
                            SWS_BILINEAR, NULL, NULL, NULL);
    if (!c->sws) return false;
    if (av_image_alloc(c->dst_data, c->dst_linesize, c->dw, c->dh, AV_PIX_FMT_BGR24, 32) < 0) {
        sws_freeContext(c->sws); c->sws = NULL; return false;
    }
    c->dst_alloc = true;
    c->sws_w = f->width; c->sws_h = f->height; c->sws_fmt = f->format;

    /* clear the screen once so letterbox bars are black */
    u16 fw, fh;
    u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fw, &fh);
    memset(fb, 0, (size_t)FB_W * SCR_W * 3);
    printf("Video: %s %s %dx%d -> %dx%d\n", avcodec_get_name(c->vctx->codec_id),
           av_get_pix_fmt_name((enum AVPixelFormat)f->format), f->width, f->height, c->dw, c->dh);
    return true;
}

static void video_render(Ctx *c, const AVFrame *f)
{
    if (!video_setup_scaler(c, f)) return;

    sws_scale(c->sws, (const uint8_t * const *)f->data, f->linesize, 0, f->height,
              c->dst_data, c->dst_linesize);

    u16 fw, fh;
    u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fw, &fh);   /* fw=240, fh=400 */
    int ox = (SCR_W - c->dw) / 2;
    int oy = (SCR_H - c->dh) / 2;

    for (int y = 0; y < c->dh; y++) {
        const uint8_t *s = c->dst_data[0] + (size_t)y * c->dst_linesize[0];   /* real stride */
        /* pixel (X,Y) of the top screen lives at ((X*240) + (239-Y)) * 3, bytes B,G,R */
        u8 *d = fb + ((size_t)ox * FB_W + (FB_W - 1 - (oy + y))) * 3;
        for (int x = 0; x < c->dw; x++) {
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
            s += 3;
            d += (size_t)FB_W * 3;
        }
    }
    gfxFlushBuffers();
    gfxSwapBuffers();
}

static double master_clock(Ctx *c)
{
    if (c->has_audio && c->ndsp_ok && c->aud_ready)
        return c->audio_base + audio_clock(&c->au);
    return c->vbase_pts + (double)(osGetTime() - c->wall0 - c->paused_ms) / 1000.0;
}

static void video_present(Ctx *c, const AVFrame *f, double pts)
{
    if (!c->have_vbase) {
        c->have_vbase = true;
        c->vbase_pts = pts;
        c->wall0 = osGetTime();
    }
    /* if audio exists but has not started yet, show immediately (no reference clock yet) */
    bool wait_ok = !(c->has_audio && c->ndsp_ok && !c->aud_ready);

    if (wait_ok) {
        int waited = 0;
        while (pts - master_clock(c) > 0.002) {
            svcSleepThread(1000000LL);
            if (!poll_input(c)) return;
            if (++waited > 500) break;          /* never stall forever on a stuck clock */
        }
        if (master_clock(c) - pts > LATE_DROP) return;   /* too late: skip rendering */
    }
    video_render(c, f);
}

static void handle_video(Ctx *c, AVPacket *pkt)
{
    if (avcodec_send_packet(c->vctx, pkt) < 0) return;
    while (avcodec_receive_frame(c->vctx, c->vframe) >= 0) {
        int64_t t = c->vframe->best_effort_timestamp;
        double pts = (t != AV_NOPTS_VALUE) ? t * av_q2d(c->vst->time_base) : c->next_vpts;
        c->next_vpts = pts + c->frame_dur;
        video_present(c, c->vframe, pts);
        av_frame_unref(c->vframe);
        if (c->quit) return;
    }
}

/* ------------------------------------------------------------------ file lookup */

static bool is_avi(const char *n)
{
    size_t l = strlen(n);
    return l > 4 && strcasecmp(n + l - 4, ".avi") == 0;
}

static bool find_avi(const char *dir, char *out, size_t cap)
{
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    bool ok = false;
    while ((e = readdir(d))) {
        if (is_avi(e->d_name)) {
            snprintf(out, cap, "%s%s", dir, e->d_name);
            ok = true;
            break;
        }
    }
    closedir(d);
    return ok;
}

static AVCodecContext *open_decoder(AVStream *st)
{
    char err[AV_ERROR_MAX_STRING_SIZE];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        say("No decoder compiled in for '%s'\n", avcodec_get_name(st->codecpar->codec_id));
        return NULL;
    }
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) return NULL;
    int r = avcodec_parameters_to_context(ctx, st->codecpar);
    if (r < 0) {
        say("avcodec_parameters_to_context failed: %d (%s)\n", r, av_make_error_string(err, sizeof(err), r));
        avcodec_free_context(&ctx);
        return NULL;
    }
    r = avcodec_open2(ctx, codec, NULL);
    if (r < 0) {
        say("avcodec_open2('%s') failed: %d (%s)\n", codec->name, r, av_make_error_string(err, sizeof(err), r));
        avcodec_free_context(&ctx);
        return NULL;
    }
    return ctx;
}

/* ------------------------------------------------------------------ opening the media */

/*
 * Why two attempts:
 * FFmpeg decides which protocol to use by parsing the text before the first ':' of the URL.
 * For "sdmc:/videos/test.avi" it reads "sdmc" as a URL scheme and looks for a protocol
 * named "sdmc", which does not exist -> AVERROR_PROTOCOL_NOT_FOUND. Prefixing "file:" forces
 * the 'file' protocol, which then hands "sdmc:/videos/test.avi" to open().
 * If the build has no 'file' protocol at all, attempt 2 reads through stdio via a custom
 * AVIOContext, which does not depend on any protocol being compiled in.
 */
static FILE         *g_fp;
static AVIOContext  *g_pb;

static int io_read(void *opaque, uint8_t *buf, int size)
{
    (void)opaque;
    size_t n = fread(buf, 1, (size_t)size, g_fp);
    if (n == 0) return feof(g_fp) ? AVERROR_EOF : AVERROR(EIO);
    return (int)n;
}

static int64_t io_seek(void *opaque, int64_t off, int whence)
{
    (void)opaque;
    if (whence & AVSEEK_SIZE) {
        long cur = ftell(g_fp);
        if (fseek(g_fp, 0, SEEK_END) != 0) return -1;
        long sz = ftell(g_fp);
        fseek(g_fp, cur, SEEK_SET);
        return sz;
    }
    whence &= ~AVSEEK_FORCE;
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) return -1;
    if (fseek(g_fp, (long)off, whence) != 0) return -1;
    return ftell(g_fp);
}

static void close_media(Ctx *c)
{
    if (c->fmt) avformat_close_input(&c->fmt);   /* does not free a custom pb */
    if (g_pb) { av_freep(&g_pb->buffer); avio_context_free(&g_pb); }
    if (g_fp) { fclose(g_fp); g_fp = NULL; }
}

static bool open_media(Ctx *c, const char *path)
{
    char err[AV_ERROR_MAX_STRING_SIZE];
    char url[600];
    int r;

    /* ---- attempt 1: normal protocol layer, forced to 'file:' */
    snprintf(url, sizeof(url), "file:%s", path);
    say("[1/3] avformat_open_input via protocol: %s\n", url);
    c->fmt = NULL;
    r = avformat_open_input(&c->fmt, url, NULL, NULL);
    if (r >= 0) {
        say("[1/3] OK (protocol layer)\n");
    } else {
        say("[1/3] FAILED avformat_open_input: %d = %s -> %s\n", r,
            av_make_error_string(err, sizeof(err), r), explain_err(r));
        c->fmt = NULL;

        /* ---- attempt 2: custom AVIOContext over stdio */
        say("[1/3] retrying with custom AVIOContext (stdio)\n");
        g_fp = fopen(path, "rb");
        if (!g_fp) {
            say("fopen('%s') FAILED: errno=%d (%s)\n", path, errno, strerror(errno));
            return false;
        }
        fseek(g_fp, 0, SEEK_END);
        long fsize = ftell(g_fp);
        fseek(g_fp, 0, SEEK_SET);
        unsigned char hdr[12] = {0};
        size_t hn = fread(hdr, 1, sizeof(hdr), g_fp);
        fseek(g_fp, 0, SEEK_SET);
        say("file size: %ld bytes; first 12 bytes: '%.4s' ... '%.4s'\n",
            fsize, hn >= 4 ? (const char *)hdr : "?", hn >= 12 ? (const char *)hdr + 8 : "?");
        if (hn < 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "AVI ", 4) != 0)
            say("WARNING: header is not RIFF....AVI  -> this file is not a standard AVI "
                "(renamed MP4/MKV, OpenDML 'AVIX' only, or truncated copy)\n");

        const int bufsz = 32768;
        unsigned char *iobuf = (unsigned char *)av_malloc(bufsz);
        if (!iobuf) { say("av_malloc failed\n"); close_media(c); return false; }
        g_pb = avio_alloc_context(iobuf, bufsz, 0, NULL, io_read, NULL, io_seek);
        if (!g_pb) { av_free(iobuf); say("avio_alloc_context failed\n"); close_media(c); return false; }

        c->fmt = avformat_alloc_context();
        if (!c->fmt) { say("avformat_alloc_context failed\n"); close_media(c); return false; }
        c->fmt->pb = g_pb;
        c->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

        r = avformat_open_input(&c->fmt, path, NULL, NULL);
        if (r < 0) {
            say("[1/3] FAILED avformat_open_input (custom AVIO): %d = %s -> %s\n", r,
                av_make_error_string(err, sizeof(err), r), explain_err(r));
            c->fmt = NULL;               /* freed by avformat_open_input on failure */
            close_media(c);
            return false;
        }
        say("[1/3] OK (custom AVIOContext)\n");
    }
    say("container detected: %s (%s)\n", c->fmt->iformat->name, c->fmt->iformat->long_name);

    /* ---- step 2: stream analysis, checked separately */
    say("[2/3] avformat_find_stream_info...\n");
    r = avformat_find_stream_info(c->fmt, NULL);
    if (r < 0) {
        say("[2/3] FAILED avformat_find_stream_info: %d = %s -> %s\n", r,
            av_make_error_string(err, sizeof(err), r), explain_err(r));
        close_media(c);
        return false;
    }
    say("[2/3] OK: %u stream(s), duration %.1f s\n", c->fmt->nb_streams,
        c->fmt->duration != AV_NOPTS_VALUE ? c->fmt->duration / (double)AV_TIME_BASE : -1.0);
    for (unsigned i = 0; i < c->fmt->nb_streams; i++) {
        const AVCodecParameters *p = c->fmt->streams[i]->codecpar;
        say("  stream %u: %s %s\n", i, av_get_media_type_string(p->codec_type),
            avcodec_get_name(p->codec_id));
    }
    return true;
}

/* ------------------------------------------------------------------ main */

int main(void)
{
    Ctx c;
    memset(&c, 0, sizeof(c));

    gfxInitDefault();
    gfxSet3D(false);
    gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);
    gfxSetDoubleBuffering(GFX_TOP, false);
    consoleInit(GFX_BOTTOM, NULL);
    osSetSpeedupEnable(true);                      /* ignored on Old 3DS */

    g_log = fopen("sdmc:/3ds/minivideo_log.txt", "w");
    av_log_set_level(AV_LOG_INFO);
    av_log_set_callback(av_log_cb);
    check_ffmpeg_build();

    char path[512];
    if (!find_avi("sdmc:/videos/", path, sizeof(path)) &&
        !find_avi("sdmc:/", path, sizeof(path)) &&
        !find_avi("sdmc:/3ds/", path, sizeof(path))) {
        printf("No .avi found in sdmc:/videos, sdmc:/ or sdmc:/3ds\nSTART to exit\n");
        goto wait_exit;
    }
    printf("File: %s\n", path);

    if (!open_media(&c, path)) {
        say("Open failed - details above and in sdmc:/3ds/minivideo_log.txt\n");
        goto wait_exit;
    }
    say("[3/3] streams ready, opening decoders\n");

    int vi = av_find_best_stream(c.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    int ai = av_find_best_stream(c.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (vi < 0) { printf("No video stream\n"); goto wait_exit; }

    c.vst  = c.fmt->streams[vi];
    c.vctx = open_decoder(c.vst);
    if (!c.vctx) { printf("Video decoder unavailable: %s\n",
                          avcodec_get_name(c.vst->codecpar->codec_id)); goto wait_exit; }

    AVRational fr = av_guess_frame_rate(c.fmt, c.vst, NULL);
    c.frame_dur = (fr.num > 0 && fr.den > 0) ? (double)fr.den / fr.num : 1.0 / 25.0;

    if (ai >= 0) {
        c.ast  = c.fmt->streams[ai];
        c.actx = open_decoder(c.ast);
        if (!c.actx) printf("Audio decoder unavailable: %s (playing muted)\n",
                            avcodec_get_name(c.ast->codecpar->codec_id));
    }
    c.has_audio = (c.actx != NULL);

    if (c.has_audio) {
        if (R_SUCCEEDED(ndspInit())) {
            c.ndsp_ok = true;
            ndspSetOutputMode(NDSP_OUTPUT_STEREO);
            audio_init_buffers(&c.au);
        } else {
            printf("NDSP init failed (sdmc:/3ds/dspfirm.cdc missing?) - no sound\n");
        }
    }

    c.vframe = av_frame_alloc();
    c.aframe = av_frame_alloc();
    c.pkt    = av_packet_alloc();
    if (!c.vframe || !c.aframe || !c.pkt) { printf("Out of memory\n"); goto wait_exit; }

    while (!c.quit && av_read_frame(c.fmt, c.pkt) >= 0) {
        if (!poll_input(&c)) { av_packet_unref(c.pkt); break; }
        if (c.pkt->stream_index == vi)                     handle_video(&c, c.pkt);
        else if (c.has_audio && c.pkt->stream_index == ai) handle_audio(&c, c.pkt);
        av_packet_unref(c.pkt);
    }
    if (!c.quit) {                                         /* flush decoders at EOF */
        handle_video(&c, NULL);
        if (c.has_audio) handle_audio(&c, NULL);
        /* let queued audio finish */
        for (int i = 0; i < 3000 && !c.quit && c.ndsp_ok && c.aud_ready; i++) {
            bool busy = false;
            for (int b = 0; b < ABUFS; b++)
                if (c.au.wb[b].status == NDSP_WBUF_QUEUED || c.au.wb[b].status == NDSP_WBUF_PLAYING)
                    busy = true;
            if (!busy) break;
            svcSleepThread(1000000LL);
            poll_input(&c);
        }
        printf("End of file. START to exit\n");
    }

wait_exit:
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }

    if (c.ndsp_ok) { ndspChnWaveBufClear(ACH); ndspExit(); }
    if (c.au.mem) linearFree(c.au.mem);
    free(c.tmp);
    if (c.dst_alloc) av_freep(&c.dst_data[0]);
    if (c.sws) sws_freeContext(c.sws);
    if (c.swr) swr_free(&c.swr);
    av_packet_free(&c.pkt);
    av_frame_free(&c.vframe);
    av_frame_free(&c.aframe);
    avcodec_free_context(&c.vctx);
    avcodec_free_context(&c.actx);
    close_media(&c);
    if (g_log) { fclose(g_log); g_log = NULL; }
    gfxExit();
    return 0;
}