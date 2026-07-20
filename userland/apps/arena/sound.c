/* Maytera Arena - sound.c
 * Sound effects. There is no tone/PCM-submit syscall in userland; the one
 * playback path is SYS_PLAY_WAV (kernel audio_play_file), and that call is
 * SYNCHRONOUS in the kernel (see kernel/main.c #702 note and the music
 * player, which spawns a helper process for the same reason).
 *
 * So: snd_init() synthesizes each SND_* effect as a short mono 16-bit WAV,
 * writes them to disk once, and starts ONE dedicated playback pthread. The
 * thread sleeps on a futex-backed pthread condvar (the shared blocking
 * primitive - no busy-wait, no poll loop) and performs the blocking
 * sys_play_wav() calls. snd_play() just enqueues an id under a mutex that
 * is only ever held for a few instructions, so the render loop NEVER
 * blocks on audio.
 */
#include "game.h"
#include "pthread.h"

#ifndef O_WRONLY
#define O_WRONLY 0x0001
#endif
#ifndef O_CREAT
#define O_CREAT  0x0040
#endif
#ifndef O_TRUNC
#define O_TRUNC  0x0200
#endif

#define SND_RATE     22050
#define SND_MAX_SAMP 9000            /* >= 400 ms at 22050 Hz */

static char           g_path[SND_NUM][40];
static int            g_ok[SND_NUM];
static int            g_inited;
static int            g_disabled;
static unsigned long  g_last_ms[SND_NUM];

/* minimum ms between two queued plays of the same effect (drop extras;
 * playback is serialized in the worker anyway, so spam only adds latency) */
static const int g_min_gap[SND_NUM] = {
    /* SHOOT   */ 100,
    /* ROCKET  */ 180,
    /* EXPLODE */ 200,
    /* RAIL    */ 160,
    /* JUMP    */ 160,
    /* PAIN    */ 180,
    /* DIE     */ 300,
    /* PICKUP  */ 140,
    /* HITBEEP */ 90,
};

/* ------------------------------------------------------------ play queue */
#define SQ_LEN 8
static int             g_q[SQ_LEN];
static unsigned        g_q_head, g_q_tail;      /* head=push, tail=pop */
static pthread_mutex_t g_q_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_q_cv  = PTHREAD_COND_INITIALIZER;

static void *snd_worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_q_mtx);
        while (g_q_head == g_q_tail)
            pthread_cond_wait(&g_q_cv, &g_q_mtx);
        int id = g_q[g_q_tail % SQ_LEN];
        g_q_tail++;
        pthread_mutex_unlock(&g_q_mtx);
        /* blocking kernel call - blocks THIS thread only */
        if (id >= 0 && id < SND_NUM && g_ok[id])
            sys_play_wav(g_path[id]);
    }
    return 0;
}

/* ------------------------------------------------------------- synthesis */
static short g_pcm[SND_MAX_SAMP];

static unsigned g_rng = 0x1234ABCD;
static inline float noise1(void) {           /* white noise in -1..1 */
    g_rng = g_rng * 1103515245u + 12345u;
    return (float)(int)((g_rng >> 16) & 0x7FFF) * (2.0f / 32767.0f) - 1.0f;
}

static inline short clamp16(float v) {
    if (v >  0.98f) v =  0.98f;
    if (v < -0.98f) v = -0.98f;
    return (short)(v * 32000.0f);
}

/* Render `id` into g_pcm; returns sample count. Every effect is a small
 * combination of: sine / square sweeps, filtered noise, and an envelope.  */
static int synth(int id) {
    int   n = 0;
    float ph = 0.0f, lp = 0.0f;
    const float TWO_PI = 2.0f * M_PI_F;

    switch (id) {
    case SND_SHOOT: {                       /* short punchy noise burst   */
        n = SND_RATE * 70 / 1000;
        for (int i = 0; i < n; i++) {
            float t   = (float)i / (float)n;
            float env = (1.0f - t) * (1.0f - t);
            lp += (noise1() - lp) * 0.55f;
            float f = 180.0f - 90.0f * t;
            ph += TWO_PI * f / SND_RATE;
            float sq = mx_sinf(ph) >= 0.0f ? 1.0f : -1.0f;
            g_pcm[i] = clamp16((lp * 0.8f + sq * 0.35f) * env);
        }
    } break;
    case SND_ROCKET: {                      /* low rumble + whoosh        */
        n = SND_RATE * 240 / 1000;
        for (int i = 0; i < n; i++) {
            float t   = (float)i / (float)n;
            float env = (t < 0.1f) ? t * 10.0f : (1.0f - t) / 0.9f;
            lp += (noise1() - lp) * 0.25f;
            float f = 110.0f - 50.0f * t;
            ph += TWO_PI * f / SND_RATE;
            float sq = mx_sinf(ph) >= 0.0f ? 1.0f : -1.0f;
            g_pcm[i] = clamp16((lp * 0.7f + sq * 0.3f) * env * 0.9f);
        }
    } break;
    case SND_EXPLODE: {                     /* big low filtered boom      */
        n = SND_RATE * 380 / 1000;
        for (int i = 0; i < n; i++) {
            float t   = (float)i / (float)n;
            float env = (1.0f - t) * (1.0f - t);
            lp += (noise1() - lp) * 0.12f;
            float f = 60.0f - 30.0f * t;
            ph += TWO_PI * f / SND_RATE;
            g_pcm[i] = clamp16((lp * 1.4f + mx_sinf(ph) * 0.5f) * env);
        }
    } break;
    case SND_RAIL: {                        /* high-to-low zap sweep      */
        n = SND_RATE * 180 / 1000;
        for (int i = 0; i < n; i++) {
            float t   = (float)i / (float)n;
            float env = 1.0f - t;
            float f   = 2200.0f - 1900.0f * t * t;
            ph += TWO_PI * f / SND_RATE;
            float s = mx_sinf(ph) + 0.2f * noise1();
            g_pcm[i] = clamp16(s * env * 0.8f);
        }
    } break;
    case SND_JUMP: {                        /* quick rising boing         */
        n = SND_RATE * 100 / 1000;
        for (int i = 0; i < n; i++) {
            float t   = (float)i / (float)n;
            float env = (t < 0.15f) ? t / 0.15f : (1.0f - t) / 0.85f;
            float f   = 260.0f + 420.0f * t;
            ph += TWO_PI * f / SND_RATE;
            g_pcm[i] = clamp16(mx_sinf(ph) * env * 0.7f);
        }
    } break;
    case SND_PAIN: {                        /* harsh short buzz           */
        n = SND_RATE * 110 / 1000;
        for (int i = 0; i < n; i++) {
            float t   = (float)i / (float)n;
            float env = 1.0f - t;
            float f   = 210.0f - 60.0f * t;
            ph += TWO_PI * f / SND_RATE;
            float sq = mx_sinf(ph) >= 0.0f ? 1.0f : -1.0f;
            g_pcm[i] = clamp16((sq * 0.6f + noise1() * 0.25f) * env);
        }
    } break;
    case SND_DIE: {                         /* long falling groan         */
        n = SND_RATE * 320 / 1000;
        for (int i = 0; i < n; i++) {
            float t   = (float)i / (float)n;
            float env = (1.0f - t);
            float f   = 320.0f - 250.0f * t;
            ph += TWO_PI * f / SND_RATE;
            float sq = mx_sinf(ph) >= 0.0f ? 1.0f : -1.0f;
            g_pcm[i] = clamp16((sq * 0.5f + mx_sinf(ph) * 0.3f) * env);
        }
    } break;
    case SND_PICKUP: {                      /* two ascending chime notes  */
        n = SND_RATE * 130 / 1000;
        for (int i = 0; i < n; i++) {
            float t   = (float)i / (float)n;
            float f   = (t < 0.5f) ? 700.0f : 1050.0f;
            float lt  = (t < 0.5f) ? t * 2.0f : (t - 0.5f) * 2.0f;
            float env = 1.0f - lt;
            ph += TWO_PI * f / SND_RATE;
            g_pcm[i] = clamp16(mx_sinf(ph) * env * 0.65f);
        }
    } break;
    case SND_HITBEEP: {                     /* tiny confirmation blip     */
        n = SND_RATE * 50 / 1000;
        for (int i = 0; i < n; i++) {
            float t   = (float)i / (float)n;
            float env = 1.0f - t;
            ph += TWO_PI * 1300.0f / SND_RATE;
            g_pcm[i] = clamp16(mx_sinf(ph) * env * 0.6f);
        }
    } break;
    default: return 0;
    }

    /* 2 ms fade-in kills the start click */
    int fade = SND_RATE * 2 / 1000;
    for (int i = 0; i < fade && i < n; i++)
        g_pcm[i] = (short)((int)g_pcm[i] * i / fade);
    return n;
}

/* --------------------------------------------------------------- WAV I/O */
static void wr_le32(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static void wr_le16(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
}

static int write_wav(const char *path, const short *pcm, int nsamp) {
    unsigned char hdr[44];
    unsigned data = (unsigned)nsamp * 2u;
    memcpy(hdr, "RIFF", 4);      wr_le32(hdr + 4, 36 + data);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    wr_le32(hdr + 16, 16);       wr_le16(hdr + 20, 1);   /* PCM       */
    wr_le16(hdr + 22, 1);                                /* mono      */
    wr_le32(hdr + 24, SND_RATE); wr_le32(hdr + 28, SND_RATE * 2);
    wr_le16(hdr + 32, 2);        wr_le16(hdr + 34, 16);  /* 16-bit    */
    memcpy(hdr + 36, "data", 4); wr_le32(hdr + 40, data);

    int fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;
    long w1 = sys_write(fd, hdr, 44);
    long w2 = sys_write(fd, pcm, data);
    sys_close(fd);
    return (w1 == 44 && w2 == (long)data) ? 0 : -1;
}

/* ==================================================================== */
void snd_init(void) {
    if (g_inited) return;
    g_inited = 1;

    /* candidate dirs, first writable wins (FAT 8.3 uppercase names) */
    static const char *dirs[] = { "/SOUNDS", "/TMP", "" };
    int wrote_any = 0;

    for (int id = 0; id < SND_NUM; id++) {
        int n = synth(id);
        g_ok[id] = 0;
        if (n <= 0 || n > SND_MAX_SAMP) continue;
        for (int d = 0; d < 3 && !g_ok[id]; d++) {
            char p[40]; p[0] = 0;
            int l = 0;
            const char *s = dirs[d];
            while (*s && l < 24) p[l++] = *s++;
            const char *base = "/ARENA";
            s = base; while (*s) p[l++] = *s++;
            p[l++] = (char)('0' + id);
            s = ".WAV"; while (*s) p[l++] = *s++;
            p[l] = 0;
            if (write_wav(p, g_pcm, n) == 0) {
                memcpy(g_path[id], p, sizeof(g_path[id]));
                g_ok[id] = 1;
                wrote_any = 1;
            }
        }
    }

    if (!wrote_any) { g_disabled = 1; return; }

    pthread_t th;
    if (pthread_create(&th, 0, snd_worker, 0) != 0)
        g_disabled = 1;           /* no worker => stay silent, never block */
}

void snd_play(int snd_id) {
    if (!g_inited || g_disabled) return;
    if (snd_id < 0 || snd_id >= SND_NUM || !g_ok[snd_id]) return;

    unsigned long now = uptime_ms();
    if (g_last_ms[snd_id] &&
        now - g_last_ms[snd_id] < (unsigned long)g_min_gap[snd_id])
        return;                                   /* rate-limit spam */

    pthread_mutex_lock(&g_q_mtx);                 /* held ~a dozen insns */
    unsigned depth = g_q_head - g_q_tail;
    int dup = 0;
    for (unsigned i = g_q_tail; i != g_q_head; i++)
        if (g_q[i % SQ_LEN] == snd_id) { dup = 1; break; }
    if (!dup && depth < SQ_LEN) {
        g_q[g_q_head % SQ_LEN] = snd_id;
        g_q_head++;
        g_last_ms[snd_id] = now;
        pthread_cond_signal(&g_q_cv);
    }
    pthread_mutex_unlock(&g_q_mtx);
}
