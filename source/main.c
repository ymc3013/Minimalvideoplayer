/*
 * Minimal AVI player - dual video version for Nintendo 3DS (devkitARM + libctru + FFmpeg)
 *
 *   top.avi -> TOP screen    (400x240 area, picture only, its audio is ignored)
 *   bot.avi -> BOTTOM screen (320x240 area, picture + THE ONLY audio)
 *
 * Files are looked up (case-insensitive) in sdmc:/videos/, sdmc:/, sdmc:/3ds/.
 * If only one of them exists, only that one is played. If NEITHER exists, the first *.avi
 * found is played on the top screen with its audio (previous single-video behaviour).
 *
 * Per video: reader thread (demux) -> packet queues -> video thread (decode+draw).
 * Bottom video additionally: audio thread (decode + NDSP).
 * The top video is synchronised on the bottom video's audio clock, so both stay together.
 *
 * Controls: A = pause/resume, START = quit.
 */

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>

#include <libavformat/avformat.h>
#include <libavformat/avio.h>
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

#define SCR_H 240
#define FB_W  240            /* framebuffer "width" = physical screen height */

#define ACH          0
#define ABUFS        16
#define ABUF_SAMPLES 16384   /* max samples per NDSP buffer (stereo S16 => 4 bytes/sample) */
#define LATE_DROP    0.150   /* seconds */

#define HAS_CHLAYOUT (LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 24, 100))

/* ------------------------------------------------------------------ types */

/* thread-safe FIFO of demuxed packets (LightLock-protected) */
typedef struct {
    AVPacket **q;
    int        cap, head, tail, count;
    size_t     bytes;
    size_t     max_bytes;
    LightLock  lock;
} PQ;

typedef struct {
    ndspWaveBuf wb[ABUFS];
    int16_t    *mem;
    int         next;
    int64_t     start[ABUFS];   /* absolute sample index at which each buffer starts */
    int64_t     queued;         /* total samples queued so far */
    double      last_clock;     /* monotonic, seconds of audio played */
    int         rate;
} Audio;

typedef struct Player {
    const char  *tag;                 /* "TOP" / "BOT" for messages */
    char         path[512];
    bool         present;             /* file found */
    bool         active;              /* opened + decoders ready */
    gfxScreen_t  screen;
    int          scr_w;               /* 400 (top) or 320 (bottom) */
    bool         want_audio;          /* only the bottom player (or legacy single video) */
    struct Player *clock_src;         /* player whose audio clock is the master, or NULL = self */

    AVFormatContext *fmt;
    int          vi, ai;
    FILE        *fp;                  /* custom AVIO fallback */
    AVIOContext *pb;

    AVCodecContext *vctx, *actx;
    AVStream    *vst, *ast;
    AVFrame     *vframe, *aframe;

    /* video scaling */
    struct SwsContext *sws;
    int       sws_w, sws_h, sws_fmt;
    uint8_t  *dst_data[4];
    int       dst_linesize[4];
    int       dw, dh;
    bool      dst_alloc;
    double    frame_dur;
    double    next_vpts;
    bool      skipping;               /* decoder skip_frame active (catching up) */

    /* audio */
    struct SwrContext *swr;
    Audio     au;
    int16_t  *tmp;
    int       tmp_cap;
    volatile bool audio_on;           /* audio really used (decoder + NDSP + setup ok) */
    volatile bool aud_ready;
    double    audio_base;

    /* fallback wall clock */
    u64       wall0;
    double    vbase_pts;
    volatile bool have_vbase;

    /* pipeline */
    PQ        vq, aq;
    Thread    th_reader, th_audio, th_video;
    volatile bool go;
    volatile bool eof;
    volatile bool audio_done;
    volatile bool video_done;
    int       dropped, shown, underruns;
} Player;

/* ------------------------------------------------------------------ globals */

static volatile bool g_quit;
static volatile bool g_paused;
static volatile u64  g_paused_ms;
static bool          g_ndsp;           /* NDSP initialised */
static volatile bool g_quiet;          /* true = bottom screen shows video, no console text */
static FILE         *g_log;
static LightLock     g_gfx_lock;

/* ------------------------------------------------------------------ logging / diagnostics */

static void say(const char *f, ...)
{
    char b[512];
    va_list v;
    va_start(v, f);
    vsnprintf(b, sizeof(b), f, v);
    va_end(v);
    /* On affiche sur l'écran du haut si on n'est pas en mode vidéo (g_quiet) */
    if (!g_quiet) fputs(b, stdout);
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
    if (!g_quiet && level <= AV_LOG_WARNING) fputs(line, stdout);
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

/* ------------------------------------------------------------------ packet queues */

static bool pq_init(PQ *p, int cap, size_t max_bytes)
{
    memset(p, 0, sizeof(*p));
    p->q = (AVPacket **)calloc((size_t)cap, sizeof(AVPacket *));
    if (!p->q) return false;
    p->cap = cap;
    p->max_bytes = max_bytes;
    LightLock_Init(&p->lock);
    return true;
}

static bool pq_full(PQ *p)
{
    if (!p->q) return true;
    LightLock_Lock(&p->lock);
    bool f = p->count >= p->cap || p->bytes >= p->max_bytes;
    LightLock_Unlock(&p->lock);
    return f;
}

static int pq_count(PQ *p)
{
    if (!p->q) return 0;
    LightLock_Lock(&p->lock);
    int n = p->count;
    LightLock_Unlock(&p->lock);
    return n;
}

static size_t pq_bytes(PQ *p)
{
    if (!p->q) return 0;
    LightLock_Lock(&p->lock);
    size_t b = p->bytes;
    LightLock_Unlock(&p->lock);
    return b;
}

/* takes ownership of the packet's data (src is left empty) */
static bool pq_push(PQ *p, AVPacket *src)
{
    AVPacket *n = av_packet_alloc();
    if (!n) return false;
    av_packet_move_ref(n, src);
    LightLock_Lock(&p->lock);
    if (p->count >= p->cap || p->bytes >= p->max_bytes) {
        LightLock_Unlock(&p->lock);
        av_packet_free(&n);
        return false;
    }
    p->q[p->tail] = n;
    p->tail = (p->tail + 1) % p->cap;
    p->count++;
    p->bytes += (size_t)n->size;
    LightLock_Unlock(&p->lock);
    return true;
}

static AVPacket *pq_pop(PQ *p)
{
    if (!p->q) return NULL;
    LightLock_Lock(&p->lock);
    if (p->count == 0) { LightLock_Unlock(&p->lock); return NULL; }
    AVPacket *n = p->q[p->head];
    p->head = (p->head + 1) % p->cap;
    p->count--;
    p->bytes -= (size_t)n->size;
    LightLock_Unlock(&p->lock);
    return n;
}

static void pq_free(PQ *p)
{
    if (!p->q) return;
    AVPacket *n;
    while ((n = pq_pop(p))) av_packet_free(&n);
    free(p->q);
    p->q = NULL;
}

/* ------------------------------------------------------------------ audio (bottom / legacy player only) */

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

/* queue n interleaved stereo S16 samples; waits (pause-safe) if all buffers are busy */
static bool audio_push(Player *p, const int16_t *src, int n)
{
    Audio *a = &p->au;
    while (n > 0) {
        ndspWaveBuf *wb = &a->wb[a->next];
        while (wb->status == NDSP_WBUF_QUEUED || wb->status == NDSP_WBUF_PLAYING) {
            if (g_quit) return false;
            if (g_paused) { svcSleepThread(10000000LL); continue; }
            svcSleepThread(2000000LL);                 /* 2 ms */
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
static bool audio_setup_from_frame(Player *p, const AVFrame *f)
{
    int rate = f->sample_rate > 0 ? f->sample_rate : p->actx->sample_rate;
    if (rate <= 0) return false;

#if HAS_CHLAYOUT
    AVChannelLayout in_l = {0}, out_l = {0};
    int nb = f->ch_layout.nb_channels > 0 ? f->ch_layout.nb_channels : p->actx->ch_layout.nb_channels;
    if (f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || f->ch_layout.nb_channels == 0)
        av_channel_layout_default(&in_l, nb > 0 ? nb : 2);
    else
        av_channel_layout_copy(&in_l, &f->ch_layout);
    av_channel_layout_default(&out_l, 2);
    p->swr = NULL;
    if (swr_alloc_set_opts2(&p->swr, &out_l, AV_SAMPLE_FMT_S16, rate,
                            &in_l, (enum AVSampleFormat)f->format, rate, 0, NULL) < 0) {
        av_channel_layout_uninit(&in_l); av_channel_layout_uninit(&out_l);
        return false;
    }
    av_channel_layout_uninit(&in_l); av_channel_layout_uninit(&out_l);
#else
    int nb = f->channels > 0 ? f->channels : p->actx->channels;
    int64_t in_l = f->channel_layout ? (int64_t)f->channel_layout : av_get_default_channel_layout(nb);
    p->swr = swr_alloc_set_opts(NULL, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, rate,
                                in_l, (enum AVSampleFormat)f->format, rate, 0, NULL);
    if (!p->swr) return false;
#endif
    if (swr_init(p->swr) < 0) { swr_free(&p->swr); return false; }

    p->au.rate = rate;
    float mix[12] = {0};
    mix[0] = mix[1] = 1.0f;
    ndspChnReset(ACH);
    ndspChnSetInterp(ACH, NDSP_INTERP_LINEAR);
    ndspChnSetRate(ACH, (float)rate);
    ndspChnSetFormat(ACH, NDSP_FORMAT_STEREO_PCM16);
    ndspChnSetMix(ACH, mix);
    say("[%s] Audio: %s %s %d Hz, %d ch -> S16 stereo\n", p->tag,
        avcodec_get_name(p->actx->codec_id),
        av_get_sample_fmt_name((enum AVSampleFormat)f->format), rate, nb);
    return true;
}

static void handle_audio(Player *p, AVPacket *pkt)
{
    if (!p->actx) return;
    if (avcodec_send_packet(p->actx, pkt) < 0) return;
    while (avcodec_receive_frame(p->actx, p->aframe) >= 0) {
        AVFrame *f = p->aframe;
        if (!p->swr) {
            if (!audio_setup_from_frame(p, f)) { p->audio_on = false; av_frame_unref(f); return; }
            int64_t t = f->best_effort_timestamp;
            p->audio_base = (t != AV_NOPTS_VALUE) ? t * av_q2d(p->ast->time_base) : 0.0;
        }
        int out_cap = swr_get_out_samples(p->swr, f->nb_samples);
        if (out_cap < f->nb_samples) out_cap = f->nb_samples;
        out_cap += 256;
        if (out_cap > p->tmp_cap) {
            free(p->tmp);
            p->tmp = (int16_t *)malloc((size_t)out_cap * 2 * sizeof(int16_t));
            p->tmp_cap = p->tmp ? out_cap : 0;
            if (!p->tmp) { av_frame_unref(f); return; }
        }
        uint8_t *outp[1] = { (uint8_t *)p->tmp };
        int n = swr_convert(p->swr, outp, p->tmp_cap,
                            (const uint8_t **)f->extended_data, f->nb_samples);
        av_frame_unref(f);
        if (n <= 0) continue;
        if (!audio_push(p, p->tmp, n)) return;
        if (!p->aud_ready) { __sync_synchronize(); p->aud_ready = true; }
    }
}

/* ------------------------------------------------------------------ video */

static bool video_setup_scaler(Player *p, const AVFrame *f)
{
    if (p->sws && p->sws_w == f->width && p->sws_h == f->height && p->sws_fmt == f->format)
        return true;

    if (p->sws) { sws_freeContext(p->sws); p->sws = NULL; }
    if (p->dst_alloc) { av_freep(&p->dst_data[0]); p->dst_alloc = false; }

    double s = (double)p->scr_w / f->width;
    double sy = (double)SCR_H / f->height;
    if (sy < s) s = sy;
    p->dw = ((int)(f->width * s)) & ~1;
    p->dh = ((int)(f->height * s)) & ~1;
    if (p->dw < 2) p->dw = 2;
    if (p->dh < 2) p->dh = 2;

    p->sws = sws_getContext(f->width, f->height, (enum AVPixelFormat)f->format,
                            p->dw, p->dh, AV_PIX_FMT_BGR24,
                            SWS_BILINEAR, NULL, NULL, NULL);
    if (!p->sws) return false;
    if (av_image_alloc(p->dst_data, p->dst_linesize, p->dw, p->dh, AV_PIX_FMT_BGR24, 32) < 0) {
        sws_freeContext(p->sws); p->sws = NULL; return false;
    }
    p->dst_alloc = true;
    p->sws_w = f->width; p->sws_h = f->height; p->sws_fmt = f->format;

    /* clear this screen once so letterbox bars are black */
    u16 fw, fh;
    u8 *fb = gfxGetFramebuffer(p->screen, GFX_LEFT, &fw, &fh);
    if (fb) memset(fb, 0, (size_t)FB_W * p->scr_w * 3);
    say("[%s] Video: %s %s %dx%d -> %dx%d\n", p->tag, avcodec_get_name(p->vctx->codec_id),
        av_get_pix_fmt_name((enum AVPixelFormat)f->format), f->width, f->height, p->dw, p->dh);
    return true;
}

static void video_render(Player *p, const AVFrame *f)
{
    if (!video_setup_scaler(p, f)) return;

    sws_scale(p->sws, (const uint8_t * const *)f->data, f->linesize, 0, f->height,
              p->dst_data, p->dst_linesize);

    /* Lock around the framebuffer access to prevent races between top and bot threads */
    LightLock_Lock(&g_gfx_lock);
    u16 fw, fh;
    u8 *fb = gfxGetFramebuffer(p->screen, GFX_LEFT, &fw, &fh);   /* fw=240 */
    if (!fb) {
        LightLock_Unlock(&g_gfx_lock);
        return;
    }

    int ox = (p->scr_w - p->dw) / 2;
    int oy = (SCR_H - p->dh) / 2;

    for (int y = 0; y < p->dh; y++) {
        const uint8_t *s = p->dst_data[0] + (size_t)y * p->dst_linesize[0];   /* real stride */
        /* pixel (X,Y) lives at ((X*240) + (239-Y)) * 3, bytes B,G,R (same for both screens) */
        u8 *d = fb + ((size_t)ox * FB_W + (FB_W - 1 - (oy + y))) * 3;
        for (int x = 0; x < p->dw; x++) {
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
            s += 3;
            d += (size_t)FB_W * 3;
        }
    }
    GSPGPU_FlushDataCache(fb, (u32)FB_W * p->scr_w * 3);
    gfxScreenSwapBuffers(p->screen, false);
    LightLock_Unlock(&g_gfx_lock);
}

static double master_clock(Player *p)
{
    Player *a = p->clock_src ? p->clock_src : p;
    if (a->audio_on && a->aud_ready)
        return a->audio_base + audio_clock(&a->au);
    if (a->have_vbase && a->wall0)
        return a->vbase_pts + (double)(osGetTime() - a->wall0 - g_paused_ms) / 1000.0;
    if (p->have_vbase && p->wall0)
        return p->vbase_pts + (double)(osGetTime() - p->wall0 - g_paused_ms) / 1000.0;
    return 0.0;
}

static bool master_ready(Player *p)
{
    Player *a = p->clock_src ? p->clock_src : p;
    if (a->audio_on && a->aud_ready) return true;
    if (a->have_vbase && a->wall0)   return true;
    if (p->have_vbase && p->wall0)   return true;
    return false;
}

static void video_present(Player *p, const AVFrame *f, double pts)
{
    if (!p->have_vbase) {
        p->have_vbase = true;
        p->vbase_pts = pts;
        p->wall0 = osGetTime();
    }

    bool wait_ok = master_ready(p);

    if (wait_ok) {
        int waited = 0;
        while (!g_quit && pts - master_clock(p) > 0.002) {
            if (g_paused) { svcSleepThread(10000000LL); continue; }
            svcSleepThread(1000000LL);
            if (++waited > 500) break;
        }
        while (g_paused && !g_quit) svcSleepThread(10000000LL);
        if (g_quit) return;
        double late = master_clock(p) - pts;
        if (late > 0.30 && !p->skipping)       { p->vctx->skip_frame = AVDISCARD_NONREF; p->skipping = true; }
        else if (late < 0.05 && p->skipping)   { p->vctx->skip_frame = AVDISCARD_DEFAULT; p->skipping = false; }
        if (late > LATE_DROP) { p->dropped++; return; }
    }
    video_render(p, f);
    p->shown++;
}

static void handle_video(Player *p, AVPacket *pkt)
{
    if (avcodec_send_packet(p->vctx, pkt) < 0) return;
    while (avcodec_receive_frame(p->vctx, p->vframe) >= 0) {
        int64_t t = p->vframe->best_effort_timestamp;
        double pts = (t != AV_NOPTS_VALUE) ? t * av_q2d(p->vst->time_base) : p->next_vpts;
        p->next_vpts = pts + p->frame_dur;
        video_present(p, p->vframe, pts);
        av_frame_unref(p->vframe);
        if (g_quit) return;
    }
}

/* ------------------------------------------------------------------ file lookup */

static bool is_avi(const char *n)
{
    size_t l = strlen(n);
    return l > 4 && strcasecmp(n + l - 4, ".avi") == 0;
}

static bool find_in_dir(const char *dir, const char *name, char *out, size_t cap)
{
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    bool ok = false;
    while ((e = readdir(d))) {
        bool match = name ? (strcasecmp(e->d_name, name) == 0) : is_avi(e->d_name);
        if (match) {
            snprintf(out, cap, "%s%s", dir, e->d_name);
            ok = true;
            break;
        }
    }
    closedir(d);
    return ok;
}

/* name == NULL -> first *.avi */
static bool find_file(const char *name, char *out, size_t cap)
{
    return find_in_dir("sdmc:/videos/", name, out, cap) ||
           find_in_dir("sdmc:/",        name, out, cap) ||
           find_in_dir("sdmc:/3ds/",    name, out, cap);
}

static AVCodecContext *open_decoder(const char *tag, AVStream *st)
{
    char err[AV_ERROR_MAX_STRING_SIZE];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        say("[%s] No decoder compiled in for '%s'\n", tag, avcodec_get_name(st->codecpar->codec_id));
        return NULL;
    }
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) return NULL;
    int r = avcodec_parameters_to_context(ctx, st->codecpar);
    if (r < 0) {
        say("[%s] avcodec_parameters_to_context failed: %d (%s)\n", tag, r, av_make_error_string(err, sizeof(err), r));
        avcodec_free_context(&ctx);
        return NULL;
    }
    r = avcodec_open2(ctx, codec, NULL);
    if (r < 0) {
        say("[%s] avcodec_open2('%s') failed: %d (%s)\n", tag, codec->name, r, av_make_error_string(err, sizeof(err), r));
        avcodec_free_context(&ctx);
        return NULL;
    }
    return ctx;
}

/* ------------------------------------------------------------------ opening the media */

static int io_read(void *opaque, uint8_t *buf, int size)
{
    Player *p = (Player *)opaque;
    size_t n = fread(buf, 1, (size_t)size, p->fp);
    if (n == 0) return feof(p->fp) ? AVERROR_EOF : AVERROR(EIO);
    return (int)n;
}

static int64_t io_seek(void *opaque, int64_t off, int whence)
{
    Player *p = (Player *)opaque;
    if (whence & AVSEEK_SIZE) {
        long cur = ftell(p->fp);
        if (fseek(p->fp, 0, SEEK_END) != 0) return -1;
        long sz = ftell(p->fp);
        fseek(p->fp, cur, SEEK_SET);
        return sz;
    }
    whence &= ~AVSEEK_FORCE;
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) return -1;
    if (fseek(p->fp, (long)off, whence) != 0) return -1;
    return ftell(p->fp);
}

static void close_media(Player *p)
{
    if (p->fmt) avformat_close_input(&p->fmt);
    if (p->pb) { av_freep(&p->pb->buffer); avio_context_free(&p->pb); }
    if (p->fp) { fclose(p->fp); p->fp = NULL; }
}

static bool open_media(Player *p)
{
    char err[AV_ERROR_MAX_STRING_SIZE];
    char url[600];
    int r;

    /* ---- attempt 1: normal protocol layer, forced to 'file:' */
    snprintf(url, sizeof(url), "file:%s", p->path);
    say("[%s] avformat_open_input via protocol: %s\n", p->tag, url);
    p->fmt = NULL;
    r = avformat_open_input(&p->fmt, url, NULL, NULL);
    if (r >= 0) {
        say("[%s] open OK (protocol layer)\n", p->tag);
    } else {
        say("[%s] FAILED avformat_open_input: %d = %s -> %s\n", p->tag, r,
            av_make_error_string(err, sizeof(err), r), explain_err(r));
        p->fmt = NULL;

        /* ---- attempt 2: custom AVIOContext over stdio */
        say("[%s] retrying with custom AVIOContext (stdio)\n", p->tag);
        p->fp = fopen(p->path, "rb");
        if (!p->fp) {
            say("[%s] fopen('%s') FAILED: errno=%d (%s)\n", p->tag, p->path, errno, strerror(errno));
            return false;
        }
        fseek(p->fp, 0, SEEK_END);
        long fsize = ftell(p->fp);
        fseek(p->fp, 0, SEEK_SET);
        unsigned char hdr[12] = {0};
        size_t hn = fread(hdr, 1, sizeof(hdr), p->fp);
        fseek(p->fp, 0, SEEK_SET);
        say("[%s] file size: %ld bytes; first 12 bytes: '%.4s' ... '%.4s'\n", p->tag,
            fsize, hn >= 4 ? (const char *)hdr : "?", hn >= 12 ? (const char *)hdr + 8 : "?");
        if (hn < 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "AVI ", 4) != 0)
            say("[%s] WARNING: header is not RIFF....AVI  -> not a standard AVI "
                "(renamed MP4/MKV, or truncated copy)\n", p->tag);

        const int bufsz = 32768;
        unsigned char *iobuf = (unsigned char *)av_malloc(bufsz);
        if (!iobuf) { say("[%s] av_malloc failed\n", p->tag); close_media(p); return false; }
        p->pb = avio_alloc_context(iobuf, bufsz, 0, p, io_read, NULL, io_seek);
        if (!p->pb) { av_free(iobuf); say("[%s] avio_alloc_context failed\n", p->tag); close_media(p); return false; }

        p->fmt = avformat_alloc_context();
        if (!p->fmt) { say("[%s] avformat_alloc_context failed\n", p->tag); close_media(p); return false; }
        p->fmt->pb = p->pb;
        p->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

        r = avformat_open_input(&p->fmt, p->path, NULL, NULL);
        if (r < 0) {
            say("[%s] FAILED avformat_open_input (custom AVIO): %d = %s -> %s\n", p->tag, r,
                av_make_error_string(err, sizeof(err), r), explain_err(r));
            p->fmt = NULL;
            close_media(p);
            return false;
        }
        say("[%s] open OK (custom AVIOContext)\n", p->tag);
    }
    say("[%s] container: %s (%s)\n", p->tag, p->fmt->iformat->name, p->fmt->iformat->long_name);

    /* ---- stream analysis, checked separately */
    r = avformat_find_stream_info(p->fmt, NULL);
    if (r < 0) {
        say("[%s] FAILED avformat_find_stream_info: %d = %s -> %s\n", p->tag, r,
            av_make_error_string(err, sizeof(err), r), explain_err(r));
        close_media(p);
        return false;
    }
    say("[%s] %u stream(s), duration %.1f s\n", p->tag, p->fmt->nb_streams,
        p->fmt->duration != AV_NOPTS_VALUE ? p->fmt->duration / (double)AV_TIME_BASE : -1.0);
    for (unsigned i = 0; i < p->fmt->nb_streams; i++) {
        const AVCodecParameters *cp = p->fmt->streams[i]->codecpar;
        say("[%s]   stream %u: %s %s\n", p->tag, i, av_get_media_type_string(cp->codec_type),
            avcodec_get_name(cp->codec_id));
    }
    return true;
}

static bool player_prepare(Player *p)
{
    if (!open_media(p)) return false;

    p->vi = av_find_best_stream(p->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    p->ai = av_find_best_stream(p->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (p->vi < 0) { say("[%s] No video stream\n", p->tag); return false; }

    p->vst  = p->fmt->streams[p->vi];
    p->vctx = open_decoder(p->tag, p->vst);
    if (!p->vctx) return false;

    AVRational fr = av_guess_frame_rate(p->fmt, p->vst, NULL);
    p->frame_dur = (fr.num > 0 && fr.den > 0) ? (double)fr.den / fr.num : 1.0 / 25.0;

    if (p->want_audio && p->ai >= 0) {
        p->ast  = p->fmt->streams[p->ai];
        p->actx = open_decoder(p->tag, p->ast);
        if (!p->actx) say("[%s] audio decoder unavailable (playing muted)\n", p->tag);
    } else if (!p->want_audio && p->ai >= 0) {
        say("[%s] audio stream present but ignored on purpose (only bottom video has sound)\n", p->tag);
    }

    p->vframe = av_frame_alloc();
    p->aframe = av_frame_alloc();
    if (!p->vframe || !p->aframe) { say("[%s] out of memory\n", p->tag); return false; }

    if (!pq_init(&p->vq, 1024, 3u << 20)) { say("[%s] out of memory (video queue)\n", p->tag); return false; }
    if (p->actx && !pq_init(&p->aq, 4096, 1536u << 10)) { say("[%s] out of memory (audio queue)\n", p->tag); return false; }

    p->active = true;
    return true;
}

/* ------------------------------------------------------------------ threads */

static void reader_thread(void *arg)
{
    Player *p = (Player *)arg;
    AVPacket *pkt = av_packet_alloc();
    while (pkt && !g_quit) {
        if (av_read_frame(p->fmt, pkt) < 0) break;
        PQ *q = NULL;
        if (pkt->stream_index == p->vi)                              q = &p->vq;
        else if (p->audio_on && pkt->stream_index == p->ai)          q = &p->aq;
        if (!q) { av_packet_unref(pkt); continue; }
        while (!g_quit && pq_full(q)) svcSleepThread(2000000LL);
        if (g_quit) { av_packet_unref(pkt); break; }
        if (!pq_push(q, pkt)) av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    __sync_synchronize();
    p->eof = true;
}

static void audio_thread(void *arg)
{
    Player *p = (Player *)arg;
    while (!p->go && !g_quit) svcSleepThread(2000000LL);
    while (!g_quit) {
        AVPacket *pk = pq_pop(&p->aq);
        if (!pk) {
            bool e = p->eof;
            pk = pq_pop(&p->aq);
            if (!pk) { if (e) break; svcSleepThread(2000000LL); continue; }
        }
        if (p->audio_on) handle_audio(p, pk);
        av_packet_free(&pk);
    }
    if (!g_quit && p->audio_on) handle_audio(p, NULL);
    __sync_synchronize();
    p->audio_done = true;
}

static void video_thread(void *arg)
{
    Player *p = (Player *)arg;
    while (!p->go && !g_quit) svcSleepThread(2000000LL);

    Player *a = p->clock_src ? p->clock_src : p;
    if (a->audio_on && a->th_audio) {
        u64 t0 = osGetTime();
        while (!g_quit && !a->aud_ready && !a->audio_done && osGetTime() - t0 < 3000)
            svcSleepThread(2000000LL);
        if (!a->aud_ready) {
            if (a == p) {
                a->audio_on = false;
                say("[%s] audio did not start within 3 s - audio disabled, using wall clock\n", p->tag);
            } else {
                say("[%s] master audio not ready after 3 s - using wall clock (master audio kept)\n", p->tag);
            }
        }
    }

    if (p->clock_src) {
        u64 t0 = osGetTime();
        while (!g_quit && !master_ready(p) && osGetTime() - t0 < 3000)
            svcSleepThread(2000000LL);
    }

    bool starving = false;
    while (!g_quit) {
        while (g_paused && !g_quit) svcSleepThread(10000000LL);
        AVPacket *pk = pq_pop(&p->vq);
        if (!pk) {
            bool e = p->eof;
            pk = pq_pop(&p->vq);
            if (!pk) {
                if (e) break;
                if (!starving) { p->underruns++; starving = true; }
                svcSleepThread(2000000LL);
                continue;
            }
        }
        starving = false;
        handle_video(p, pk);
        av_packet_free(&pk);
    }
    if (!g_quit) handle_video(p, NULL);
    __sync_synchronize();
    p->video_done = true;
}

static Thread make_thread(ThreadFunc fn, void *arg, size_t stack, int prio, int core)
{
    Thread t = threadCreate(fn, arg, stack, prio, core, false);
    if (!t) t = threadCreate(fn, arg, stack, prio, 0, false);
    return t;
}

static void player_free(Player *p)
{
    if (p->th_audio)  { threadJoin(p->th_audio,  U64_MAX); threadFree(p->th_audio);  p->th_audio  = NULL; }
    if (p->th_video)  { threadJoin(p->th_video,  U64_MAX); threadFree(p->th_video);  p->th_video  = NULL; }
    if (p->th_reader) { threadJoin(p->th_reader, U64_MAX); threadFree(p->th_reader); p->th_reader = NULL; }
    pq_free(&p->vq);
    pq_free(&p->aq);
    if (p->au.mem) { linearFree(p->au.mem); p->au.mem = NULL; }
    free(p->tmp); p->tmp = NULL;
    if (p->dst_alloc) { av_freep(&p->dst_data[0]); p->dst_alloc = false; }
    if (p->sws) { sws_freeContext(p->sws); p->sws = NULL; }
    if (p->swr) swr_free(&p->swr);
    av_frame_free(&p->vframe);
    av_frame_free(&p->aframe);
    avcodec_free_context(&p->vctx);
    avcodec_free_context(&p->actx);
    close_media(p);
}

/* ------------------------------------------------------------------ main */

int main(void)
{
    static Player pl[2];
    memset(pl, 0, sizeof(pl));
    pl[0].tag = "TOP"; pl[0].screen = GFX_TOP;    pl[0].scr_w = 400; pl[0].want_audio = false;
    pl[1].tag = "BOT"; pl[1].screen = GFX_BOTTOM; pl[1].scr_w = 320; pl[1].want_audio = true;

    gfxInitDefault();
    gfxSet3D(false);

    /* CONFIGURATION VIDÉO : On force le format BGR8 pour les deux écrans.
       On le fait AVANT d'initialiser la console pour que la console du haut s'adapte. */
    gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);
    gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);
    gfxSetDoubleBuffering(GFX_TOP, false);
    gfxSetDoubleBuffering(GFX_BOTTOM, false);

    /* CONSOLE DE LOG : On met la console sur le HAUT pour ne pas détruire le framebuffer du bas.
       Le printf() et say() écriront ici pendant le chargement. */
    consoleInit(GFX_TOP, NULL);
    consoleClear();

    osSetSpeedupEnable(true);
    LightLock_Init(&g_gfx_lock);

    g_log = fopen("sdmc:/3ds/minivideo_log.txt", "w");
    av_log_set_level(AV_LOG_INFO);
    av_log_set_callback(av_log_cb);
    check_ffmpeg_build();

    pl[0].present = find_file("top.avi", pl[0].path, sizeof(pl[0].path));
    pl[1].present = find_file("bot.avi", pl[1].path, sizeof(pl[1].path));
    bool legacy = false;
    if (!pl[0].present && !pl[1].present) {
        if (find_file(NULL, pl[0].path, sizeof(pl[0].path))) {
            pl[0].present = true;
            pl[0].want_audio = true;
            legacy = true;
        }
    }
    if (!pl[0].present && !pl[1].present) {
        say("No top.avi / bot.avi / *.avi found in sdmc:/videos, sdmc:/ or sdmc:/3ds\n");
        goto wait_exit;
    }
    say("Mode: %s\n", legacy ? "single video (no top.avi/bot.avi found)" : "top.avi / bot.avi");
    if (pl[0].present) say("TOP file: %s\n", pl[0].path);
    if (pl[1].present) say("BOT file: %s\n", pl[1].path);

    for (int i = 0; i < 2; i++) {
        if (!pl[i].present) continue;
        if (!player_prepare(&pl[i])) {
            say("[%s] could not be prepared - skipped (details above / in log)\n", pl[i].tag);
            pl[i].active = false;
        }
    }
    if (!pl[0].active && !pl[1].active) {
        say("Nothing to play - see details above and in sdmc:/3ds/minivideo_log.txt\n");
        goto wait_exit;
    }

    {
        Player *ap = NULL;
        for (int i = 0; i < 2; i++)
            if (pl[i].active && pl[i].want_audio && pl[i].actx) { ap = &pl[i]; break; }
        if (ap) {
            if (R_SUCCEEDED(ndspInit())) {
                g_ndsp = true;
                ndspSetOutputMode(NDSP_OUTPUT_STEREO);
                audio_init_buffers(&ap->au);
                ap->audio_on = true;
            } else {
                say("NDSP init failed (sdmc:/3ds/dspfirm.cdc missing?) - no sound\n");
            }
        }
        if (pl[0].active && pl[1].active && pl[1].audio_on) pl[0].clock_src = &pl[1];
    }

    {
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        int wprio = prio - 1;
        if (wprio < 0x19) wprio = 0x19;
        bool n3ds = false;
        APT_CheckNew3DS(&n3ds);
        if (!n3ds) APT_SetAppCpuTimeLimit(30);
        int wcore = n3ds ? 2 : 1;
        int vcore[2] = { n3ds ? 2 : 0, 0 };
        bool ok = true;

        for (int i = 0; i < 2; i++) {
            Player *p = &pl[i];
            if (!p->active) continue;
            p->th_reader = make_thread(reader_thread, p, 128 * 1024, wprio, wcore);
            p->th_video  = make_thread(video_thread,  p, 512 * 1024, prio,  vcore[i]);
            if (p->audio_on) p->th_audio = make_thread(audio_thread, p, 256 * 1024, wprio, wcore);
            if (!p->th_reader || !p->th_video || (p->audio_on && !p->th_audio)) ok = false;
        }
        if (!ok) { say("threadCreate failed (cannot run pipeline)\n"); goto wait_exit; }
        say("threads started (%s)\n", n3ds ? "New 3DS family" : "Old 3DS");
    }

    {
        u64 tb0 = osGetTime();
        while (!g_quit) {
            if (!aptMainLoop()) { g_quit = true; break; }
            hidScanInput();
            if (hidKeysDown() & KEY_START) { g_quit = true; break; }

            bool all_ok = true;
            for (int i = 0; i < 2; i++) {
                Player *p = &pl[i];
                if (!p->active) continue;
                int pre = (int)(1.5 / p->frame_dur + 0.5);
                if (pre < 12) pre = 12;
                if (pre > 120) pre = 120;
                bool vok = pq_count(&p->vq) >= pre || p->eof || pq_full(&p->vq);
                bool aok = !p->audio_on || pq_count(&p->aq) > 0 || p->eof;
                if (!(vok && aok)) all_ok = false;
                /* Affichage de chargement sur le HAUT */
                printf("\x1b[%d;1H%s buffering %3d/%3d   ", 27 + i, p->tag, pq_count(&p->vq), pre);
            }
            fflush(stdout);
            if (all_ok) break;
            if (osGetTime() - tb0 > 10000) { say("\nprebuffer timeout (10 s)\n"); break; }
            svcSleepThread(20000000LL);
        }
        say("\nPrebuffered in %u ms\n", (unsigned)(osGetTime() - tb0));
        for (int i = 0; i < 2; i++)
            if (pl[i].active)
                say("[%s] queue: video %d pkts (%u KB), audio %d pkts\n", pl[i].tag,
                    pq_count(&pl[i].vq), (unsigned)(pq_bytes(&pl[i].vq) >> 10), pq_count(&pl[i].aq));
    }

    /* Dès que la lecture commence, on cache le texte pour ne pas gêner la vidéo */
    if (!g_quit) {
        g_quiet = true;
        consoleClear(); 
    }

    __sync_synchronize();
    for (int i = 0; i < 2; i++) pl[i].go = true;

    {
        u64 tstat = osGetTime(), tlog = tstat, pause_start = 0;
        while (!g_quit) {
            if (!aptMainLoop()) { g_quit = true; break; }
            hidScanInput();
            u32 kd = hidKeysDown();
            if (kd & KEY_START) { g_quit = true; break; }
            if (kd & KEY_A) {
                if (!g_paused) {
                    pause_start = osGetTime();
                    g_paused = true;
                    if (g_ndsp) ndspChnSetPaused(ACH, true);
                } else {
                    g_paused_ms += osGetTime() - pause_start;
                    if (g_ndsp) ndspChnSetPaused(ACH, false);
                    __sync_synchronize();
                    g_paused = false;
                }
            }

            bool done = true;
            for (int i = 0; i < 2; i++) {
                Player *p = &pl[i];
                if (!p->active) continue;
                if (!p->video_done) done = false;
                if (p->th_audio && !p->audio_done) done = false;
            }
            if (done) break;

            u64 now = osGetTime();
            if (now - tstat >= 1000) {
                tstat = now;
                if (now - tlog >= 5000) {
                    tlog = now;
                    for (int i = 0; i < 2; i++) {
                        Player *p = &pl[i];
                        if (!p->active) continue;
                        say("[%s] V queue:%d A queue:%d shown:%d dropped:%d starved:%d\n", p->tag,
                            pq_count(&p->vq), pq_count(&p->aq), p->shown, p->dropped, p->underruns);
                    }
                }
            }
            svcSleepThread(16000000LL);
        }
    }

    if (!g_quit) {
        for (int pi = 0; pi < 2; pi++) {
            if (!pl[pi].audio_on || !pl[pi].aud_ready) continue;
            for (int i = 0; i < 3000 && !g_quit; i++) {
                bool busy = false;
                for (int b = 0; b < ABUFS; b++)
                    if (pl[pi].au.wb[b].status == NDSP_WBUF_QUEUED ||
                        pl[pi].au.wb[b].status == NDSP_WBUF_PLAYING) busy = true;
                if (!busy) break;
                svcSleepThread(1000000LL);
            }
        }
    }

    if (g_quiet) {
        g_quiet = false;
        consoleClear();
    }
    for (int i = 0; i < 2; i++)
        if (pl[i].active)
            say("[%s] end: shown=%d dropped=%d starved=%d times\n", pl[i].tag,
                pl[i].shown, pl[i].dropped, pl[i].underruns);
    if (!g_quit) say("End of playback. START to exit\n");

wait_exit:
    while (!g_quit && aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }

    g_quit = true;
    for (int i = 0; i < 2; i++) player_free(&pl[i]);
    if (g_ndsp) { ndspChnWaveBufClear(ACH); ndspExit(); }
    if (g_log) { fclose(g_log); g_log = NULL; }
    gfxExit();
    return 0;
}