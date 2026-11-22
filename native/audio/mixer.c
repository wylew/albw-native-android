/* Native voice mixer (see audio.h). One call to aud_mix_frame() = one 3DS sound frame:
 * 160 samples at 32728 Hz. Voices decode their wave buffers on demand (PCM8 / PCM16 /
 * DSP-ADPCM), resample with cubic (polyphase setting), linear or no interpolation, run the
 * optional one-pole and biquad filters, apply the voice volume and the 12 mix gains, and
 * accumulate into the main stereo mix and the two 4-channel aux buses. */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>

#include "audio.h"

typedef struct {
    /* parameters */
    int state;
    int fmt, channels;
    int32_t rate;
    float pitch, volume;
    float mix[12];
    int interp;
    int16_t coefs[16];
    bool mono_en;
    float mono_a, mono_y;
    bool bq_en;
    float bq[5];
    float bx1, bx2, by1, by2;
    /* wave buffer queue (ring) */
    AudWaveBuf q[AUD_QUEUE_MAX];
    int qhead, qcount;
    bool started;          /* head buffer has started (PLAY reported) */
    uint32_t pos;          /* next sample of the head buffer to decode */
    int32_t yn1, yn2;      /* ADPCM history */
    /* resampler */
    float h[4];
    double frac;
    bool primed;
} Voice;

typedef struct {
    float ret;
    AudAuxFn fn;       /* written under g_aux_mu + g_mu */
    void *user;
    uintptr_t user2;
    AudFx *fx[2];
} AuxBus;

static pthread_mutex_t g_mu;
static pthread_mutex_t g_aux_mu;   /* held while user aux callbacks run */
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static Voice g_v[AUD_VOICES];
static AuxBus g_aux[AUD_AUX_BUSES];
static float g_master = 1.0f;
static AudStatusFn g_status_fn;
static uint64_t g_frames;

static void init_mutex(pthread_mutex_t *m) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
}

static void voice_defaults(Voice *v) {
    memset(v, 0, sizeof *v);
    v->state = AUD_STATE_PAUSE;
    v->fmt = AUD_FMT_PCM16;
    v->channels = 1;
    v->rate = AUD_RATE;
    v->pitch = 1.0f;
    v->volume = 1.0f;
    v->mix[AUD_CH_FL] = 1.0f;   /* nn::snd default MixParam: front left/right 1.0, rest 0 */
    v->mix[AUD_CH_FR] = 1.0f;
    v->interp = AUD_INTERP_POLYPHASE;
}

/* save states (native/runtime/savestate.c; weak: tools without it): the voices and the master
 * volume (the aux buses hold host effect objects and keep their current state) */
extern void albw_ss_add_range(const char *name, void *p, size_t n) __attribute__((weak));

static void once_init(void) {
    init_mutex(&g_mu);
    init_mutex(&g_aux_mu);
    for (int i = 0; i < AUD_VOICES; i++) voice_defaults(&g_v[i]);
    for (int b = 0; b < AUD_AUX_BUSES; b++) g_aux[b].ret = 1.0f;
    if (albw_ss_add_range) {
        albw_ss_add_range("mixer voices", g_v, sizeof g_v);
        albw_ss_add_range("mixer master", &g_master, sizeof g_master);
    }
}

void aud_init(void) { pthread_once(&g_once, once_init); }
void aud_lock(void) { aud_init(); pthread_mutex_lock(&g_mu); }
void aud_unlock(void) { pthread_mutex_unlock(&g_mu); }

void aud_set_status_callback(AudStatusFn fn) {
    aud_lock();
    g_status_fn = fn;
    aud_unlock();
}

static void report(const AudWaveBuf *b, int status) {
    if (g_status_fn) g_status_fn(b->user, status);
}

static bool valid(int v) { return v >= 0 && v < AUD_VOICES; }

/* ------------------------------------------------------------------ queue */
static void queue_flush(Voice *v, bool notify) {
    while (v->qcount > 0) {
        if (notify) report(&v->q[v->qhead], AUD_WB_DONE);
        v->qhead = (v->qhead + 1) % AUD_QUEUE_MAX;
        v->qcount--;
    }
    v->qhead = 0;
    v->started = false;
    v->pos = 0;
    v->primed = false;
    v->frac = 0;
    memset(v->h, 0, sizeof v->h);
    v->mono_y = 0;
    v->bx1 = v->bx2 = v->by1 = v->by2 = 0;
}

static void queue_pop(Voice *v) {
    v->qhead = (v->qhead + 1) % AUD_QUEUE_MAX;
    v->qcount--;
    v->started = false;
    v->pos = 0;
}

/* ------------------------------------------------------------------ decoding */
static float decode(Voice *v, const AudWaveBuf *b, uint32_t pos) {
    int ch = v->channels == 2 ? 2 : 1;
    switch (v->fmt) {
    case AUD_FMT_PCM8: {
        const int8_t *d = (const int8_t *)b->data + (size_t)pos * ch;
        return ch == 2 ? (float)(d[0] + d[1]) * 128.0f : (float)d[0] * 256.0f;
    }
    case AUD_FMT_PCM16: {
        const int16_t *d = (const int16_t *)b->data + (size_t)pos * ch;
        return ch == 2 ? (float)(d[0] + d[1]) * 0.5f : (float)d[0];
    }
    case AUD_FMT_ADPCM: {
        /* DSP-ADPCM: 8-byte frames = 1 header byte (predictor << 4 | scale) + 14 nibbles */
        const uint8_t *frame = (const uint8_t *)b->data + (size_t)(pos / 14) * 8;
        uint32_t i = pos % 14;
        int hdr = frame[0];
        int scale = 1 << (hdr & 0xF);
        int pi = (hdr >> 4) & 7;
        int byte = frame[1 + i / 2];
        int nib = (i & 1) ? (byte & 0xF) : (byte >> 4);
        if (nib >= 8) nib -= 16;
        int32_t s = (((nib * scale) << 11) + 1024 +
                     v->coefs[pi * 2] * v->yn1 + v->coefs[pi * 2 + 1] * v->yn2) >> 11;
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        v->yn2 = v->yn1;
        v->yn1 = s;
        return (float)s;
    }
    }
    return 0.0f;
}

static void start_buffer(Voice *v, const AudWaveBuf *b) {
    v->pos = 0;
    if (v->fmt == AUD_FMT_ADPCM && b->has_ctx) {
        v->yn1 = b->ctx.yn1;
        v->yn2 = b->ctx.yn2;
    }
}

/* next decoded sample of the voice; 0 when the queue is empty (the voice keeps running) */
static float fetch(Voice *v) {
    for (int guard = 0; guard < AUD_QUEUE_MAX + 2 && v->qcount > 0; guard++) {
        AudWaveBuf *b = &v->q[v->qhead];
        if (!v->started) {
            v->started = true;
            start_buffer(v, b);
            report(b, AUD_WB_PLAY);
        }
        if (v->pos < b->length) return decode(v, b, v->pos++);
        if (b->loop && b->length > 0) {
            start_buffer(v, b);
            return decode(v, b, v->pos++);
        }
        report(b, AUD_WB_DONE);
        queue_pop(v);
    }
    return 0.0f;
}

/* ------------------------------------------------------------------ voice API */
void aud_voice_reset(int v) {
    if (!valid(v)) return;
    aud_lock();
    voice_defaults(&g_v[v]);
    aud_unlock();
}

void aud_voice_set_state(int v, int state) {
    if (!valid(v)) return;
    aud_lock();
    Voice *x = &g_v[v];
    if (getenv("ALBW_MIX_DIAG"))
        fprintf(stderr, "mixdiag: voice %d state %d -> %d vol %g mix %g %g %g %g pitch %g rate %u fmt %d q %d primed %d mono %d a %g\n", v,
                x->state, state, x->volume, x->mix[0], x->mix[1], x->mix[4], x->mix[8], x->pitch, (unsigned)x->rate, x->fmt,
                (int)x->qcount, (int)x->primed, (int)x->mono_en, x->mono_a);
    if (state == AUD_STATE_STOP) queue_flush(x, true);
    x->state = state;
    aud_unlock();
}

int aud_voice_get_state(int v) { return valid(v) ? g_v[v].state : AUD_STATE_STOP; }

#define SETTER(name, type, field)                \
    void name(int v, type val) {                 \
        if (!valid(v)) return;                   \
        aud_lock();                              \
        g_v[v].field = val;                      \
        aud_unlock();                            \
    }
SETTER(aud_voice_set_format, int, fmt)
SETTER(aud_voice_set_channels, int, channels)
SETTER(aud_voice_set_rate, int32_t, rate)
SETTER(aud_voice_set_volume, float, volume)
SETTER(aud_voice_set_interp, int, interp)

void aud_voice_set_pitch(int v, float pitch) {
    if (!valid(v)) return;
    if (!(pitch >= 0.0f)) pitch = 0.0f;   /* also NaN */
    aud_lock();
    g_v[v].pitch = pitch;
    aud_unlock();
}

void aud_voice_set_mix(int v, const float mix[12]) {
    if (!valid(v)) return;
    aud_lock();
    memcpy(g_v[v].mix, mix, sizeof g_v[v].mix);
    aud_unlock();
}

void aud_voice_set_adpcm_coefs(int v, const int16_t coefs[16]) {
    if (!valid(v)) return;
    aud_lock();
    memcpy(g_v[v].coefs, coefs, sizeof g_v[v].coefs);
    aud_unlock();
}

void aud_voice_set_mono_filter(int v, bool enable, uint32_t cutoff_hz) {
    if (!valid(v)) return;
    aud_lock();
    Voice *x = &g_v[v];
    x->mono_en = enable;
    if (cutoff_hz) {
        /* one-pole low-pass y = (1-a) x + a y[-1], pole from the cut-off frequency */
        double w = 2.0 * M_PI * (double)cutoff_hz / (double)AUD_RATE_EXACT;
        double c = 2.0 - cos(w);
        x->mono_a = (float)(c - sqrt(c * c - 1.0));
        if (!(x->mono_a >= 0.0f && x->mono_a < 1.0f))
        {
            uint16_t sw, tw; uint32_t env[7];
#if defined(__i386__) || defined(__x86_64__)
            __asm__ volatile("fnstenv %0" : "=m"(env)); __asm__ volatile("fldenv %0" :: "m"(env));
#else
            memset(env, 0, sizeof env);
#endif
            sw = (uint16_t)env[1]; tw = (uint16_t)env[2];
            fprintf(stderr, "mixer: voice %d one-pole cut-off %u -> a %g (x87 status %04x top %d tags %04x)\n", v,
                    (unsigned)cutoff_hz, x->mono_a, sw, (sw >> 11) & 7, tw);
        }
    }
    aud_unlock();
}

void aud_voice_set_biquad(int v, bool enable, const int16_t coef[5]) {
    if (!valid(v)) return;
    aud_lock();
    Voice *x = &g_v[v];
    x->bq_en = enable;
    if (coef)
        for (int i = 0; i < 5; i++) x->bq[i] = (float)coef[i] / 16384.0f;
    aud_unlock();
}

int aud_voice_append(int v, const AudWaveBuf *wb) {
    if (!valid(v)) return -1;
    aud_lock();
    Voice *x = &g_v[v];
    int r = -1;
    if (x->qcount < AUD_QUEUE_MAX) {
        x->q[(x->qhead + x->qcount) % AUD_QUEUE_MAX] = *wb;
        x->qcount++;
        r = 0;
    }
    aud_unlock();
    return r;
}

uint32_t aud_voice_play_position(int v) {
    if (!valid(v)) return 0;
    aud_lock();
    uint32_t p = g_v[v].pos;
    p = p > 3 ? p - 3 : 0;   /* the resampler decodes up to 3 samples ahead */
    aud_unlock();
    return p;
}

bool aud_voice_busy(int v) { return valid(v) && g_v[v].qcount > 0; }

/* ------------------------------------------------------------------ global */
void aud_set_master_volume(float vol) {
    aud_lock();
    g_master = vol >= 0.0f ? vol : 0.0f;
    aud_unlock();
}
float aud_get_master_volume(void) { return g_master; }

void aud_set_aux_return(int bus, float vol) {
    if (bus < 0 || bus >= AUD_AUX_BUSES) return;
    aud_lock();
    g_aux[bus].ret = vol;
    aud_unlock();
}
float aud_get_aux_return(int bus) {
    return (bus >= 0 && bus < AUD_AUX_BUSES) ? g_aux[bus].ret : 0.0f;
}

void aud_set_aux_callback(int bus, AudAuxFn fn, void *user, uintptr_t user2) {
    if (bus < 0 || bus >= AUD_AUX_BUSES) return;
    aud_init();
    pthread_mutex_lock(&g_aux_mu);     /* waits for a running callback to return */
    aud_lock();
    g_aux[bus].fn = fn;
    g_aux[bus].user = user;
    g_aux[bus].user2 = user2;
    aud_unlock();
    pthread_mutex_unlock(&g_aux_mu);
}

void aud_get_aux_callback(int bus, void **user, uintptr_t *user2) {
    if (bus < 0 || bus >= AUD_AUX_BUSES) return;
    aud_lock();
    if (user) *user = g_aux[bus].fn ? g_aux[bus].user : NULL;
    if (user2) *user2 = g_aux[bus].fn ? g_aux[bus].user2 : 0;
    aud_unlock();
}

void aud_set_aux_fx(int bus, int slot, const AudFxConfig *cfg) {
    if (bus < 0 || bus >= AUD_AUX_BUSES || slot < 0 || slot > 1) return;
    AudFx *n = (cfg && cfg->type != AUD_FX_NONE) ? aud_fx_create(cfg) : NULL;
    aud_lock();
    AudFx *old = g_aux[bus].fx[slot];
    g_aux[bus].fx[slot] = n;
    aud_unlock();
    aud_fx_destroy(old);
}

uint64_t aud_frame_count(void) { return g_frames; }

/* ------------------------------------------------------------------ mixing */
static inline float interp(const Voice *v, float f) {
    const float *h = v->h;
    switch (v->interp) {
    case AUD_INTERP_NONE:
        return h[1];
    case AUD_INTERP_LINEAR:
        return h[1] + (h[2] - h[1]) * f;
    default: { /* 4-point cubic Hermite (stands in for the DSP's polyphase filter) */
        float c1 = 0.5f * (h[2] - h[0]);
        float c2 = h[0] - 2.5f * h[1] + 2.0f * h[2] - 0.5f * h[3];
        float c3 = 0.5f * (h[3] - h[0]) + 1.5f * (h[1] - h[2]);
        return ((c3 * f + c2) * f + c1) * f + h[1];
    }
    }
}

static void mix_voice(Voice *v, float *main_l, float *main_r, float aux[2][4][AUD_FRAME],
                      const bool aux_on[2]) {
    if (v->state != AUD_STATE_PLAY) return;
    if (!v->primed) {
        if (v->qcount == 0) return;
        v->h[0] = 0.0f;
        v->h[1] = fetch(v);
        v->h[2] = fetch(v);
        v->h[3] = fetch(v);
        v->frac = 0;
        v->primed = true;
    } else if (v->qcount == 0 && v->h[0] == 0.0f && v->h[1] == 0.0f && v->h[2] == 0.0f &&
               v->h[3] == 0.0f) {
        return;   /* idle: nothing queued, history drained */
    }
    double step = (double)v->rate / (double)AUD_RATE_EXACT * (double)v->pitch;
    if (step > 64.0) step = 64.0;
    if (!(step >= 0.0)) step = 0.0;
    float gl = v->volume * (v->mix[0] + v->mix[2]);
    float gr = v->volume * (v->mix[1] + v->mix[3]);
    for (int i = 0; i < AUD_FRAME; i++) {
        float s = interp(v, (float)v->frac);
        v->frac += step;
        while (v->frac >= 1.0) {
            v->frac -= 1.0;
            v->h[0] = v->h[1];
            v->h[1] = v->h[2];
            v->h[2] = v->h[3];
            v->h[3] = fetch(v);
        }
        if (v->mono_en) {
            v->mono_y = (1.0f - v->mono_a) * s + v->mono_a * v->mono_y;
            s = v->mono_y;
        }
        if (v->bq_en) {
            /* y = b0 x + b1 x1 + b2 x2 + a1 y1 + a2 y2 (the SDK passes a1/a2 already negated) */
            float y = v->bq[0] * s + v->bq[1] * v->bx1 + v->bq[2] * v->bx2 + v->bq[3] * v->by1 +
                      v->bq[4] * v->by2;
            if (!(fabsf(y) < 1.0e6f)) y = 0.0f, v->by1 = v->by2 = 0.0f;   /* unstable: reset */
            v->bx2 = v->bx1;
            v->bx1 = s;
            v->by2 = v->by1;
            v->by1 = y;
            s = y;
        }
        main_l[i] += s * gl;
        main_r[i] += s * gr;
        for (int b = 0; b < 2; b++) {
            if (!aux_on[b]) continue;
            const float *g = &v->mix[4 + 4 * b];
            for (int c = 0; c < 4; c++) aux[b][c][i] += s * v->volume * g[c];
        }
    }
}

static int32_t to_i32(float x) {
    if (x > 2.0e9f) return 2000000000;
    if (x < -2.0e9f) return -2000000000;
    return (int32_t)lrintf(x);
}

void aud_mix_frame(float *out) {
    static float ml[AUD_FRAME], mr[AUD_FRAME];
    static float aux[2][4][AUD_FRAME];
    static int32_t auxi[2][4][AUD_FRAME];
    aud_lock();
    memset(ml, 0, sizeof ml);
    memset(mr, 0, sizeof mr);
    memset(aux, 0, sizeof aux);
    bool aux_on[2];
    for (int b = 0; b < 2; b++) aux_on[b] = g_aux[b].fn || g_aux[b].fx[0] || g_aux[b].fx[1];
    static int diag = -1;
    if (diag < 0) { const char *e = getenv("ALBW_MIX_DIAG"); diag = e && *e && *e != '0'; }
    for (int i = 0; i < AUD_VOICES; i++) {
        if (!diag) { mix_voice(&g_v[i], ml, mr, aux, aux_on); continue; }
        float before = ml[AUD_FRAME - 1];
        static float tmp[AUD_FRAME];
        memcpy(tmp, ml, sizeof tmp);
        mix_voice(&g_v[i], ml, mr, aux, aux_on);
        {
            static double ve[AUD_VOICES]; static uint64_t vlast;
            for (int k = 0; k < AUD_FRAME; k++) { double d = ml[k] - tmp[k]; ve[i] += d * d; }
            if (i == AUD_VOICES - 1 && g_frames - vlast >= 40) {
                char buf[1024]; int o = 0;
                for (int q = 0; q < AUD_VOICES && o < 900; q++)
                    if (ve[q] > 1e6) o += snprintf(buf + o, sizeof buf - o, " v%d=%.0f(vol %.2f mix %.2f)", q, sqrt(ve[q] / (40.0 * AUD_FRAME)), g_v[q].volume, g_v[q].mix[0]);
                if (o) fprintf(stderr, "mixdiag: frame %llu voice rms:%s\n", (unsigned long long)g_frames, buf);
                memset(ve, 0, sizeof ve); vlast = g_frames;
            }
        }
        static int told[AUD_VOICES];
        for (int k = 0; k < AUD_FRAME; k++)
            if (!(fabsf(ml[k]) < 200000.0f) && told[i] < 3) {
                told[i]++;
                Voice *v = &g_v[i];
                fprintf(stderr, "mixdiag: frame %llu voice %d out %g (before %g) vol %g mix %g %g pitch %g rate %u fmt %d bq %d mono %d h %g %g %g %g mono_a %g mono_y %g frac %g by %g %g\n",
                        (unsigned long long)g_frames, i, ml[k], before, v->volume, v->mix[0], v->mix[1], v->pitch,
                        (unsigned)v->rate, (int)v->fmt, (int)v->bq_en, (int)v->mono_en, v->h[0], v->h[1], v->h[2], v->h[3], v->mono_a, v->mono_y, v->frac, v->by1, v->by2);
                break;
            }
    }

    for (int b = 0; b < 2; b++) {
        if (!aux_on[b]) continue;
        int32_t *ch[4];
        for (int c = 0; c < 4; c++) {
            ch[c] = auxi[b][c];
            for (int i = 0; i < AUD_FRAME; i++) ch[c][i] = to_i32(aux[b][c][i]);
        }
        for (int s = 0; s < 2; s++)
            if (g_aux[b].fx[s]) aud_fx_process(g_aux[b].fx[s], ch, AUD_FRAME);
        if (g_aux[b].fn) {
            /* user callback: run without the engine lock (game code may call the API or take
             * its own locks); the aux lock keeps it registered until it returns */
            int depth = 0;
            while (pthread_mutex_unlock(&g_mu) == 0) depth++;   /* fully release (recursive) */
            pthread_mutex_lock(&g_aux_mu);
            AudAuxFn fn = g_aux[b].fn;
            if (fn) fn(b, ch, AUD_FRAME, g_aux[b].user, g_aux[b].user2);
            pthread_mutex_unlock(&g_aux_mu);
            while (depth-- > 0) pthread_mutex_lock(&g_mu);
        }
        float r = g_aux[b].ret;
        if (diag) {
            static int told_aux[2];
            for (int i = 0; i < AUD_FRAME && told_aux[b] < 3; i++)
                if (abs(ch[AUD_CH_FL][i]) > 200000 || !(r == r)) {
                    told_aux[b]++;
                    fprintf(stderr, "mixdiag: frame %llu aux %d returns %d (ret %g, fn %p fx %p %p)\n",
                            (unsigned long long)g_frames, b, ch[AUD_CH_FL][i], r, (void *)g_aux[b].fn,
                            (void *)g_aux[b].fx[0], (void *)g_aux[b].fx[1]);
                    break;
                }
        }
        if (diag) {
            static double e_dry, e_aux[2]; static uint64_t last;
            for (int i = 0; i < AUD_FRAME; i++) {
                if (b == 0) e_dry += (double)ml[i] * ml[i];
                double a = (double)(ch[AUD_CH_FL][i] + (int64_t)ch[AUD_CH_RL][i]) * r;
                e_aux[b] += a * a;
            }
            if (b == 0 && g_frames - last >= 160) {
                fprintf(stderr, "mixdiag: energy dry %.3g aux0 %.3g aux1 %.3g (ret %g %g)\n", e_dry, e_aux[0], e_aux[1],
                        g_aux[0].ret, g_aux[1].ret);
                e_dry = e_aux[0] = e_aux[1] = 0; last = g_frames;
            }
        }
        for (int i = 0; i < AUD_FRAME; i++) {
            ml[i] += (float)(ch[AUD_CH_FL][i] + (int64_t)ch[AUD_CH_RL][i]) * r;
            mr[i] += (float)(ch[AUD_CH_FR][i] + (int64_t)ch[AUD_CH_RR][i]) * r;
        }
    }
    float g = g_master / 32768.0f;
    for (int i = 0; i < AUD_FRAME; i++) {
        float l = ml[i] * g, r = mr[i] * g;
        out[2 * i] = l > 1.0f ? 1.0f : (l < -1.0f ? -1.0f : l);
        out[2 * i + 1] = r > 1.0f ? 1.0f : (r < -1.0f ? -1.0f : r);
    }
    g_frames++;
    aud_unlock();
}
