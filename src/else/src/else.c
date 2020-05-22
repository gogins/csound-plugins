/*
   else.c

  Copyright (C) 2019 Eduardo Moguillansky

  This file is part of Csound.

  The Csound Library is free software; you can redistribute it
  and/or modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  Csound is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with Csound; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
  02110-1301 USA

  */

/*

  # crackle

    aout  crackle kp

    generates noise based on a chaotic equation
    y[n] = p * y[n-1]- y[n-2] - 0.05

    kp: the p parameter in the equation, a value between 1.0 and 2.0

  # ramptrig

    a resetable line going from 0 to 1

    kout             ramptrig ktrig, kdur, ivaluepost=1
    aout             ramptrig ktrig, kdur, ivaluepost=1
    kout, kfinished  ramptrig ktrig, kdur, ivaluepost=1

    ktrig: whenever ktrig > 0 and ktrig > previous trig the outvalue is reset to 0
    kdur: duration of the ramp (how long does it take to go from 0 to 1)
    ivaluepost: value after reaching end of ramp (1)
    ivaluepre: value before any trigger (0)

    kout: the ramp value. Will go from 0 to 1 in kdur. If the ramp reaches 1,
        kout will be set to ivaluepost (1). Before any trigger, value is ivaluepre (0)

    Main usecase is in connection to bpf, to produce something similar to Env and EnvGen
    in supercollider

    kphase ramptrig ktrig, idur
    kenv bpf kphase * idur, 0, 0, 0.01, 1, idur, 0
    asig = pinker() * interp(kenv)

  # ramptrig

    kout ramptrig ktrig, idur, [ivaloff]

    A ramp from 0 to 1 in idur seconds. Can be retriggered. Concieved to be used together
    with bpf to generate trigerrable complex envelopes

    idur = 2
    kramp ramptrig ktrig, idur
    aenv bpf kramp*idur, 0, 0, 0.3, 1, 1.7, 1, 2, 0

  # sigmdrive  (sigmoid drive)

    aout sigmdrive ain, xfactor, kmode=0

    xfactor: drive factor (k or a)
    kmode: 0 = tanh, 1 = pow

  # lfnoise

    aout lfnoise kfreq, kinterp=0

    low frequency, band-limited noise

  # schmitt

    A schmitt trigger

    kout schmitt kin, khigh, klow
    aout schmitt ain, khigh, klow

    output value is either 1 if in > high, or 0 if in < low


  # standardchaos

    Standard map chaotic generator, the sound is generated with the difference equations;

    y[n] = (y[n-1] + k * sin(x[n-1])) % 2pi;
    x[n] = (x[n-1] + y[n]) % 2pi;
    out = (x[n] - pi) / pi;

    aout standardchaos krate, kk=1, ix=0.5, iy=0

    krate: from 0 to nyquist
    kk: a value for k in the above equation
    ix: initial value for x
    iy: initial value for y

  # linenv

    an envelope generator similar to linsegr but with retriggerable gate and
    flexible sustain point

    aout/kout rampgate kgate, isustidx, kval0, kdel0, kval1, kdel1, ..., kvaln

    kgate: when kgate changes from 0 to 1, value follows envelope until isustpoint is reached
        value stays there until kgate switches to 0, after which it follow the rest
        of the envelope and stays at the env until a new switch. If kgate switches to 0
        before reaching isustpoint, then it uses the current value as sustain point and
        follows the envelope from there. If kgate switches to 1 before reaching the
        end of the release part, it glides to the beginning and starts attack envelope
        from there
    isustidx: the idx of the sustaining value. if isustidx is 2, then the value stays
        at kval2 until kgate is closed (0).
        * Use 0 to disable sustain, negative indexes count from end (-1 is the last segment)
        NB: the first segment (index 0 ) can't be used as sustain segment. In order to have
        a sustain segment just at the beginning, use a very short first segment and set
        the sustain index to 1

        kdel0, kval0, ...: the points of the envelope, similar to linseg

        If noteoff release is needed, xtratim and release opcodes are needed.

        inspired by else's envgen

        Example: generate an adsr envelope with attack=0.05, decay=0.1, sust=0.2, rel=0.5

        isustidx = 2
        aout linenv kgate, isustidx, 0, 0.05, 1, 0.1, 0.2, 0.5, 0

        Example 2: emulate ramptrig

        These are the same:
        kout ramptrig ktrig, idur
            kout rampgate ktrig, -1, 0, idur, 1

  # sp_peaklim

    aout sp_peaklim ain, ktresh=0, iattack=0.01, irelease=0.1

  # diode_ringmod

    A ring modulator with some dirtyness. Two versions: one where an inbuilt sinus as
    modulator signal; and a second where the user passed its own signal as modulator. In the
    second case the effect of nonlinearities is reduced to feedback (in the first, the freq.
    of the oscillator is also modified)

    (mod -> diode sim -> feedback) * carrier

    aout dioderingmod acarr, kmodfreq, kdiode=0, kfeedback=0,
                      knonlinearities=0.1, koversample=0

    A port of jsfx Loser's ringmodulator

  # atstop

    schedule an instrument when the note stops

    NB: this is scheduled not at release start, but when the
    note is deallocated

    atstop Sintr, idelay, idur, pfields...
    atstop instrnum, idelay, idur, pfields...

*/

#include "csdl.h"
#include <math.h>
#include <unistd.h>
#include <ctype.h>

#define min(x, y) (((x) < (y)) ? (x) : (y))
#define max(x, y) (((x) > (y)) ? (x) : (y))

#define MSGF(fmt, ...) (csound->Message(csound, fmt, __VA_ARGS__))
#define MSG(s) (csound->Message(csound, s))
#define INITERR(m) (csound->InitError(csound, "%s", m))
#define INITERRF(fmt, ...) (csound->InitError(csound, fmt, __VA_ARGS__))
#define PERFERR(m) (csound->PerfError(csound, &(p->h), "%s", m))
#define PERFERRF(fmt, ...) (csound->PerfError(csound, &(p->h), fmt, __VA_ARGS__))

#define Uint32_tMAX 0x7FFFFFFF

#define ONE_OVER_PI 0.3183098861837907

// #define DEBUG

#ifdef DEBUG
    #define DBG(fmt, ...) printf(">>>> "fmt"\n", __VA_ARGS__); fflush(stdout);
    #define DBG_(m) DBG("%s", m)
#else
    #define DBG(fmt, ...)
    #define DBG_(m)
#endif


#define SAMPLE_ACCURATE(out) \
    uint32_t n, nsmps = CS_KSMPS;                                    \
    uint32_t offset = p->h.insdshead->ksmps_offset;                  \
    uint32_t early = p->h.insdshead->ksmps_no_end;                   \
    if (UNLIKELY(offset)) memset(out, '\0', offset*sizeof(MYFLT));   \
    if (UNLIKELY(early)) {                                           \
        nsmps -= early;                                              \
        memset(&out[nsmps], '\0', early*sizeof(MYFLT));              \
    }                                                                \


#define register_deinit(csound, p, func) \
    csound->RegisterDeinitCallback(csound, p, (int32_t(*)(CSOUND*, void*))(func))


#ifdef USE_DOUBLE
// from https://www.gamedev.net/forums/topic/621589-extremely-fast-sin-approximation/
static inline double fast_sin(double x) {
    int k;
    double y, z;
    z  = x;
    z *= 0.3183098861837907;
    z += 6755399441055744.0;
    k  = *((int *) &z);
    z  = k;
    z *= 3.1415926535897932;
    x -= z;
    y  = x;
    y *= x;
    z  = 0.0073524681968701;
    z *= y;
    z -= 0.1652891139701474;
    z *= y;
    z += 0.9996919862959676;
    x *= z;
    k &= 1;
    k += k;
    z  = k;
    z *= x;
    x -= z;
    return x;
}
#else
float fast_sin(float floatx) {
    return (float)fast_sin((double)floatx);
}
#endif

// ------------------ pool --------------------

// this is a data type to allocate slots for handles
typedef struct {
    int size;
    int capacity;
    int cangrow;
    int *data;
} intarray_t;

static intarray_t*
intpool_init(CSOUND *csound, intarray_t *pool, int size) {
    pool->size = size;
    pool->capacity = size;
    pool->cangrow = 1;
    pool->data = csound->Malloc(csound, sizeof(int) * size);
    for(int i=0; i<size; i++) {
        pool->data[i] = i;
    }
    return pool;
}

static void
intarray_resize(CSOUND *csound, intarray_t *pool, int size) {
    csound->ReAlloc(csound, pool->data, size);
    pool->capacity = size;
}

static int
intpool_push(CSOUND *csound, intarray_t *pool, int val) {
    if(pool->size >= pool->capacity) {
        return INITERR("Pool full, can't push this element");
    }
    pool->data[pool->size] = val;
    pool->size += 1;
    return OK;
}

static int
intpool_pop(CSOUND *csound, intarray_t *pool) {
    if(pool->size == 0) {
        if(!pool->cangrow) {
            INITERR("This pool is empty");
            return -1;
        }
        // pool is empty. we refill it with new values outside of the current range
        for(int i=0; i<pool->capacity; i++) {
            pool->data[i] = i+pool->capacity;
        }
        pool->size = pool->capacity;
        // and resize to make room for the values outside the pool to be pushed back
        intarray_resize(csound, pool, pool->capacity * 2);
    }
    int val = pool->data[pool->size - 1];
    pool->size--;
    return val;
}

// ----------------- opcode A ------------------

typedef struct {
    OPDS h;
    MYFLT *aout;
    MYFLT *kp;
    MYFLT y1;
    MYFLT y2;
} CRACKLE;

static inline MYFLT rnd31(CSOUND *csound, int*seed) {
    return (MYFLT) (csound->Rand31(seed) - 1) / FL(2147483645.0);
}

static int32_t crackle_init(CSOUND *csound, CRACKLE* p) {
    MYFLT kp = *p->kp;
    if (kp < 0) {
        *p->kp = 1.5;
    }
    int seed = csound->GetRandomSeedFromTime();
    p->y1 = rnd31(csound, &seed);
    // printf("y1 = %f\n", p->y1);
    p->y2 = 0;
    return OK;
}

static int32_t crackle_perf(CSOUND *csound, CRACKLE* p) {
    IGN(csound);
    MYFLT *out = p->aout;                                             \

    SAMPLE_ACCURATE(out)

    MYFLT y0;
    MYFLT kp = *p->kp;
    MYFLT y1 = p->y1;
    MYFLT y2 = p->y2;

    for(n=offset; n<nsmps; n++) {
        y0 = fabs(y1 * kp - y2 - FL(0.05));
        y2 = y1; y1 = y0;
        out[n] = y0;
    }
    p->y1 = y1;
    p->y2 = y2;

    return OK;
}


// ramptrig
// kout ramptrig ktrig, kdur, ivalpost=1, ivalpre=0
// ramp from 0 to 1 in kdur seconds, sets output to ivaloff when finished

typedef struct {
    OPDS h;
    MYFLT *out;
    MYFLT *ktrig;
    MYFLT *kdur;
    MYFLT *ivalpost;
    MYFLT ivalpre;
    MYFLT lasttrig;
    MYFLT now;
    MYFLT sr;
    int started;
} RAMPTRIGK;


static int32_t ramptrig_k_kk_init(CSOUND *csound, RAMPTRIGK *p) {
    p->lasttrig = 0;
    p->sr = csound->GetKr(csound);
    p->now = 0;
    p->started = 0;
    p->ivalpre = 0;
    return OK;
}


static int32_t ramptrig_k_kk(CSOUND *csound, RAMPTRIGK *p) {
    IGN(csound);
    MYFLT ktrig = *p->ktrig;
    MYFLT now = p->now;
    MYFLT delta = 1 / (*p->kdur * p->sr);
    if(ktrig > 0 && ktrig > p->lasttrig) {
        p->started = 1;
        p->now = 0;
    } else if(!p->started) {
        *p->out = p->ivalpre;
    } else if(now < 1) {
        *p->out = now;
        p->now += delta;
    } else {
        *p->out = *p->ivalpost;
    }
    p->lasttrig = ktrig;
    return OK;
}

static int32_t ramptrig_a_kk_init(CSOUND *csound, RAMPTRIGK *p) {
    p->lasttrig = 0;
    p->sr = csound->GetSr(csound);
    p->now = 0;
    p->started = 0;
    p->ivalpre = 0;
    return OK;
}


static int32_t ramptrig_a_kk(CSOUND *csound, RAMPTRIGK *p) {
    IGN(csound);
    MYFLT *out = p->out;

    SAMPLE_ACCURATE(out)

    MYFLT ktrig = *p->ktrig;
    MYFLT now = p->now;
    MYFLT delta = 1 / (*p->kdur * p->sr);
    if(ktrig > 0 && ktrig > p->lasttrig) {
        p->started = 1;
        p->now = 0;
    } else if(!p->started) {
        MYFLT ivalpre = p->ivalpre;
        for(n=offset; n<nsmps; n++) {
            out[n] = ivalpre;
        }
    } else if(now < 1) {
        for(n=offset; n<nsmps; n++) {
            out[n] = now;
            now += delta;
        }
        p->now = now;
    } else {
        MYFLT ivalpost = *p->ivalpost;
        for(n=offset; n<nsmps; n++)
            out[n] = ivalpost;
    }
    p->lasttrig = ktrig;
    return OK;
}


// aout, afinished ramptrig atrig, kdur
typedef struct {
    OPDS h;
    MYFLT *out;
    MYFLT *sync;
    MYFLT *trig;
    MYFLT *dur;
    MYFLT *ivalpost;
    MYFLT ivalpre;
    MYFLT lasttrig;
    MYFLT lastsync;
    MYFLT now;
    MYFLT sr;
    int started;
} RAMPTRIGSYNC;


static int32_t ramptrigsync_kk_kk_init(CSOUND *csound, RAMPTRIGSYNC *p) {
    p->lasttrig = 0;
    p->now = 0;
    p->sr = csound->GetKr(csound);
    p->lastsync = 0;
    p->started = 0;
    p->ivalpre = 0;
    return OK;
}

static int32_t ramptrigsync_kk_kk(CSOUND *csound, RAMPTRIGSYNC *p) {
    IGN(csound);
    MYFLT ktrig = *p->trig;
    MYFLT now = p->now;
    MYFLT delta = 1 / (*p->dur * p->sr);
    if(ktrig > 0 && ktrig > p->lasttrig) {
        p->started = 1;
        p->now = 0;
    } else if(!p->started) {
        *p->out = p->ivalpre;
        *p->sync = 0;
    } else if(now < 1) {
        *p->out = now;
        p->now += delta;
        *p->sync = 0;
        p->lastsync = 0;
    } else {
        *p->out = *p->ivalpost;
        *p->sync = 1 - p->lastsync;
        p->lastsync = 1;
    }
    p->lasttrig = ktrig;
    return OK;
}


// sigmdrive
// aout sigmdrive ain, xdrivefactor, kmode=0

typedef struct {
    OPDS h;
    MYFLT *out;
    MYFLT *in;
    MYFLT *drivefactor;
    MYFLT *mode;

} SIGMDRIVE;

static int32_t sigmdrive_a_ak(CSOUND *csound, SIGMDRIVE * p) {
    IGN(csound);
    MYFLT *in = p->in;
    MYFLT *out = p->out;
    MYFLT drivefactor = *p->drivefactor;
    if(drivefactor < 0)
        drivefactor = 0;
    MYFLT x;
    SAMPLE_ACCURATE(out);
    if(*p->mode == 0.) {
        for(n=offset; n<nsmps; n++) {
            out[n] = tanh(in[n] * drivefactor);
        }
    } else {
        for(n=offset; n<nsmps; n++) {
            x = in[n];
            if(x >= 1.)
                out[n] = 1.;
            else if (x <= -1)
                out[n] = 1.0;
            else if (drivefactor < 1)
                out[n] = x * drivefactor;
            else if (x > 0)
                out[n] = 1.0 - pow(1. - x, drivefactor);
            else
                out[n] = pow(1.0 + x, drivefactor) - 1.0;
        }
    }
    return OK;
}


static int32_t sigmdrive_a_aa(CSOUND *csound, SIGMDRIVE * p) {
    IGN(csound);
    MYFLT *in = p->in;
    MYFLT *out = p->out;
    MYFLT *drivefactorp = p->drivefactor;
    MYFLT drivefactor;
    MYFLT x;
    SAMPLE_ACCURATE(out);
    int mode = (int)*p->mode;
    if(mode == 0) {
        for(n=offset; n<nsmps; n++) {
            drivefactor = drivefactorp[n];
            if(drivefactor < 0)
                drivefactor = 0;
            out[n] = tanh(in[n] * drivefactor);
        }
    } else {
        for(n=offset; n<nsmps; n++) {
            x = in[n];
            drivefactor = drivefactorp[n];
            if(drivefactor < 0)
                drivefactor = 0;
            if(x >= 1.)
                out[n] = 1.;
            else if (x <= -1)
                out[n] = 1.0;
            else if (drivefactor < 1)
                out[n] = x * drivefactor;
            else if (x > 0)
                out[n] = 1.0 - pow(1. - x, drivefactor);
            else
                out[n] = pow(1.0 + x, drivefactor) - 1.0;
        }
    }
    return OK;
}


// lfnoise
// aout lfnoise kfreq, kinterp=0

typedef struct {
    OPDS h;
    MYFLT *out;
    MYFLT *freq;
    MYFLT *interp;

    MYFLT x_freq;
    int   x_val;
    int x_interp;
    double x_phase;
    MYFLT x_ynp1;
    MYFLT x_yn;
    MYFLT x_sr;
} LFNOISE;


static int32_t lfnoise_init(CSOUND *csound, LFNOISE *p) {
    int init_seed = (int)csound->GetRandomSeedFromTime();
    // static int init_seed = 234599;
    init_seed *= 1319;
    // default parameters
    MYFLT hz = *p->freq;
    int seed = init_seed;
    int interp = 0;
    if (hz >= 0)
        p->x_phase = 1.;
    p->x_freq = hz;
    p->x_interp = interp != 0;
    // get 1st output
    p->x_ynp1 = (((MYFLT)((seed & 0x7fffffff) - 0x40000000)) * (MYFLT)(1.0 / 0x40000000));
    p->x_val = seed * 435898247 + 382842987;
    p->x_sr = csound->GetSr(csound);
    return OK;
}

static int32_t lfnoise_perf(CSOUND *csound, LFNOISE *p) {
    IGN(csound);
    MYFLT *out = p->out;

    SAMPLE_ACCURATE(out);

    MYFLT hz = *p->freq;
    int interp = (int)*p->interp;
    int val = p->x_val;
    double phase = p->x_phase;
    MYFLT ynp1 = p->x_ynp1;
    MYFLT yn = p->x_yn;
    MYFLT sr = p->x_sr;
    MYFLT random;
    for(n=offset; n<nsmps; n++) {
        double phase_step = hz / sr;
        // clipped phase_step
        phase_step = phase_step > 1 ? 1. : phase_step < -1 ? -1 : phase_step;
        if (hz >= 0) {
            if (phase >= 1.) {    // update
                random = ((MYFLT)((val & 0x7fffffff) - 0x40000000)) * (MYFLT)(1.0 / 0x40000000);
                val = val * 435898247 + 382842987;
                phase = phase - 1;
                yn = ynp1;
                ynp1 = random; // next random value
            }
        } else {
           if (phase <= 0.) {    // update
               random = ((MYFLT)((val & 0x7fffffff) - 0x40000000)) * (MYFLT)(1.0 / 0x40000000);
               val = val * 435898247 + 382842987;
               phase = phase + 1;
               yn = ynp1;
               ynp1 = random; // next random value
           }
        }
        if (interp) {
            if (hz >= 0)
                *out++ = yn + (ynp1 - yn) * (phase);
            else
                *out++ = yn + (ynp1 - yn) * (1 - phase);
        }
        else {
            out[n] = yn;
        }

        phase += phase_step;
   }
   p->x_val = val;
   p->x_phase = phase;
   p->x_ynp1 = ynp1;   // next random value
   p->x_yn = yn;       // current output
   return OK;
}


// schmitt
// kout schmitt kin, khigh, klow

typedef struct {
    OPDS h;
    MYFLT *out;
    MYFLT *in;
    MYFLT *high;
    MYFLT *low;
    MYFLT last;
} SCHMITT;

static int32_t schmitt_k_init (CSOUND *csound, SCHMITT *p) {
    IGN(csound);
    p->last =0;
    return OK;
}

static int32_t schmitt_k_perf (CSOUND *csound, SCHMITT *p) {
    IGN(csound);
    MYFLT last = p->last;
    MYFLT in = *p->in;
    MYFLT lo = *p->low;
    MYFLT hi = *p->high;
    *p->out = last = (in > lo && (in >= hi || last));
    p->last = last;
    return OK;
}

static int32_t schmitt_a_perf (CSOUND *csound, SCHMITT *p) {
    IGN(csound);
    MYFLT *out = p->out;
    MYFLT x;
    SAMPLE_ACCURATE(out);

    MYFLT *in = p->in;
    MYFLT lo = *p->low;
    MYFLT hi = *p->high;
    MYFLT last = p->last;

    for(n=offset; n<nsmps; n++) {
        x = in[n];
        out[n] = last = (x > lo && (x >= hi || last));
    }
    p->last = last;
    return OK;
}

// standardchaos
// aout standardchaos krate, kk=1, ix=0.5, iy=0

typedef struct {
    OPDS h;
    MYFLT *out;
    MYFLT *rate;
    MYFLT *k;
    MYFLT *x;
    MYFLT *y;

    int x_val;
    MYFLT  x_sr;
    MYFLT  x_lastout;
    MYFLT  x_phase;
    MYFLT  x_freq;
    MYFLT  x_yn;
    MYFLT  x_xn;
} STANDARDCHAOS;


static int32_t standardchaos_init(CSOUND *csound, STANDARDCHAOS *p) {
    p->x_sr = csound->GetSr(csound);
    MYFLT hz = p->x_sr * 0.5; // default parameters
    p->x_phase = hz >= 0 ? 1 : 0;
    p->x_freq  = hz;
    p->x_lastout = 0;
    p->x_xn = *p->x;
    p->x_yn = *p->y;
    return OK;
}

static int32_t standardchaos_init_x(CSOUND *csound, STANDARDCHAOS *p) {
    *p->x = 0.5;
    *p->y = 0;
    return standardchaos_init(csound, p);
}

MYFLT mfmod(MYFLT x, MYFLT y) {
    // double mfmod(double x,double y) { double a; return ((a=x/y)-(int)a)*y; }
    MYFLT a = x/y;
    return a - (int)a * y;
}

static int32_t standardchaos_perf(CSOUND *csound, STANDARDCHAOS *p) {
    IGN(csound);

    MYFLT *out = p->out;

    SAMPLE_ACCURATE(out);

    MYFLT hz = *p->rate;
    MYFLT yn = p->x_yn;
    MYFLT xn = p->x_xn;
    MYFLT k = *p->k;
    MYFLT lastout = p->x_lastout;
    MYFLT phase = p->x_phase;
    MYFLT sr = p->x_sr;
    for(n=offset; n<nsmps; n++) {
        MYFLT phase_step = hz / sr; // phase_step
        phase_step = phase_step > 1 ? 1. : phase_step < -1 ? -1 : phase_step; // clipped phase_step
        int trig;
        MYFLT output;
        if (hz >= 0) {
            trig = phase >= 1.;
            if (phase >= 1.) phase = phase - 1;
        } else {
            trig = (phase <= 0.);
            if (phase <= 0.) phase = phase + 1.;
        }
        if (trig) {
            yn = mfmod(yn + k * fast_sin(xn), TWOPI);
            xn = mfmod(xn + yn, TWOPI);
            if (xn < 0) xn = xn + TWOPI;
            output = lastout = (xn - PI) * ONE_OVER_PI;
        }
        else output = lastout; // last output
        out[n] = output;
        phase += phase_step;
    }
    p->x_phase = phase;
    p->x_lastout = lastout;
    p->x_yn = yn;
    p->x_xn = xn;
    return OK;
}

// aout rampgate kgate, isustpoint, kval0, kdel0, kval1, kdel1, ..., kvaln
// kout rampgate kgate, isustpoint, kval0, kdel0, kval1, kdel1, ..., kvaln

// kvalx... value in seconds


#define MAXPOINTS 128

enum RampgateState { Off, Attack, Sustain, Release, Retrigger };


typedef struct {
    OPDS h;
    MYFLT *out;
    MYFLT *gate;
    MYFLT *sustpoint;
    MYFLT *points[MAXPOINTS];

    MYFLT sr;
    int lastgate;
    enum RampgateState state;
    uint32_t numpoints;

    MYFLT val;
    MYFLT t;
    MYFLT segment_end;
    MYFLT deltat;
    MYFLT prev_val;
    MYFLT next_val;
    MYFLT retrigger_ramptime;
    int32_t numsegments;
    int32_t segment_idx;
    int32_t sustain_idx;

} RAMPGATE;


static int32_t rampgate_k_init_common(CSOUND *csound, RAMPGATE *p, MYFLT sr) {
    p->sr = sr;
    p->state = Off;  // not playing
    p->numpoints = p->INOCOUNT - 2;  // this should be odd
    if(p->numpoints % 2 == 0) {
        INITERRF(Str("Number of points should be odd (got %d points)"), p->numpoints);
    }
    p->numsegments = (p->numpoints - 1) / 2;
    // printf("numpoints: %d,  numsegments: %d\n", p->numpoints, p->numsegments);
    p->lastgate = 0;
    p->t = 0;
    p->deltat = 1/p->sr;
    p->val = *(p->points[0]);
    p->segment_end = *(p->points[1]);
    p->prev_val = p->val;
    p->next_val = *(p->points[2]);
    p->segment_idx = 0;
    p->retrigger_ramptime = 0.020;  // this could be configurable
    p->sustain_idx = (int32_t)*p->sustpoint;
    int sustain_idx = (int32_t)*p->sustpoint;
    if(sustain_idx < 0) {
        sustain_idx += p->numsegments;
    }
    p->sustain_idx = sustain_idx;
    if(p->sustain_idx != 0 && (p->sustain_idx < 0 || p->sustain_idx >= p->numsegments)) {
        return INITERRF("Sustain point (%d) out of range. There are %d defined segments",
                        p->sustain_idx, p->numsegments);
    }
    return OK;
}


static int32_t linenv_k_init(CSOUND *csound, RAMPGATE *p) {
    return rampgate_k_init_common(csound, p, csound->GetKr(csound));
}


static inline void rampgate_update_segment(RAMPGATE *p, int32_t idx) {
    int32_t idx0 = idx*2;
    // we assume that p->t has been updated already
    p->segment_end = *(p->points[idx0+1]);
    p->prev_val = *(p->points[idx0]);
    p->next_val = *(p->points[idx0+2]);
    p->segment_idx = idx;
}


static int32_t linenv_k_k(CSOUND *csound, RAMPGATE *p) {
    int gate = (int)*p->gate;
    int lastgate = p->lastgate;
    MYFLT val;
    if(gate != lastgate) {
        if(gate == 1) {
            // are we playing?
            if(p->state == Off) {
                // not playing, just waiting for a gate, so start attack
                p->t = 0;
                // p->state = p->sustain_idx == 0 ? Sustain : Attack;
                p->state = Attack;
                rampgate_update_segment(p, 0);
                p->val = p->prev_val;
            } else if(p->state == Release) {
                // still playing release section, enter ramp to beginning state
                p->t = 0;
                p->state = Retrigger;
                p->prev_val = p->val;
                p->next_val = *p->points[0];
                p->segment_end = p->retrigger_ramptime;
                p->segment_idx = -1;
            } else {
                return PERFERRF("This should not happen. state = %d", p->state);
            }
        } else {
            if(p->state == Off) {
                p->t = 0;
                rampgate_update_segment(p, 0);
            } else
                p->state = Release;
            // printf("closing gate, state now is: %d\n", p->state);

        }
    }
    p->lastgate = gate;
    if(p->state == Off || p->state == Sustain) {
        // either not playing or sustaining, just output our current value
        *p->out = p->val;
        return OK;
    }

    val = (p->next_val - p->prev_val) * (p->t / p->segment_end) + p->prev_val;
    *p->out = p->val = val;
    p->t += p->deltat;

    if(p->t < p->segment_end)
        return OK;

    // we are finished with this segment
    if (p->state == Retrigger) {
        // finished retrigger state
        p->t -= p->segment_end;
        rampgate_update_segment(p, 0);
        p->val = *(p->points[0]);
        // p->state = p->sustain_idx == 0 ? Sustain : Attack;
        p->state = Attack;
    } else if(p->segment_idx >= p->numsegments - 1) {
        // end of envelope
        p->state = Off;
        p->t = 0;
        p->val = p->next_val;
        // rampgate_update_segment(p, 0);
        return OK;
    } else {
        // new segment
        p->t -= p->segment_end;
        rampgate_update_segment(p, p->segment_idx+1);
        if(p->segment_idx == p->sustain_idx && p->state == Attack) {
            // sustaining section
            p->state = Sustain;
            p->val = p->prev_val;
        }
    }
    return OK;
}


static int32_t linenv_a_init(CSOUND *csound, RAMPGATE *p) {
    return rampgate_k_init_common(csound, p, csound->GetSr(csound));
}


static int32_t linenv_a_k(CSOUND *csound, RAMPGATE *p) {
    MYFLT *out = p->out;

    SAMPLE_ACCURATE(out);

    int gate = (int)*p->gate;
    int lastgate = p->lastgate;
    MYFLT **points = p->points;

    if(gate != lastgate) {
        if(gate == 1) {  // gate is opening
            if(p->state == Off) {
                // not playing, just waiting for a gate, so start attack
                p->t = 0;
                p->prev_val = *points[0];
                p->segment_end = *points[1];
                p->next_val = *points[2];
                p->segment_idx = 0;
                // p->state = p->sustain_idx == 0 ? Sustain : Attack;
                p->state = Attack;
                p->val = p->prev_val;
            } else if(p->state == Release) {
                // still playing release section, enter ramp to beginning state
                p->t = 0;
                p->state = Retrigger;
                p->prev_val = p->val;
                p->next_val = *p->points[0];
                p->segment_end = p->retrigger_ramptime;
                p->segment_idx = -1;
            } else
                return PERFERRF("This should not happen. state = %d", p->state);
        } else {  // gate is closing
            if(p->state == Off) {
                p->t = 0;
                rampgate_update_segment(p, 0);
            } else
                p->state = Release;
        }
    }
    p->lastgate = gate;
    MYFLT t = p->t;
    enum RampgateState state = p->state;
    MYFLT val = p->val;

    // if state is Off or Sustain, state can only change with a gate, so will not change
    // during this period
    if(state == Off || state == Sustain) {
        for(n=offset; n<nsmps; n++)
            out[n] = val;
        return OK;
    }

    MYFLT next_val = p->next_val;
    MYFLT prev_val = p->prev_val;
    MYFLT segment_end = p->segment_end;
    MYFLT deltat = p->deltat;
    int32_t segment_idx = p->segment_idx;
    int32_t last_segment_idx = p->numsegments - 1;
    uint32_t m;

    for(n=offset; n<nsmps; n++) {
        val = (next_val - prev_val) * (t / segment_end) + prev_val;
        out[n] = val;
        t += deltat;
        if(t < p->segment_end)
            continue;

        // we are finished with this segment
        if (state == Retrigger) {
            // finished retrigger state
            if(p->sustain_idx == 0) {
                // landed in Sustain section
                val = *points[0];
                for(m=n; m<nsmps; m++)
                    out[m] = val;
                break;
            }
            state = Attack;
            t -= segment_end;
            segment_end = *points[1];
            prev_val = val;
            next_val = *points[2];
            segment_idx = 0;
        } else if (segment_idx < last_segment_idx) {
            // new segment
            t -= segment_end;
            prev_val = next_val;
            segment_idx += 1;
            next_val = *points[segment_idx*2+2];
            segment_end = *points[segment_idx*2+1];

            if(segment_idx == p->sustain_idx && state == Attack) {
                // sustaining section
                state = Sustain;
                val = prev_val;
            }
        } else {
            // end of envelope
            state = Off;
            t = 0;
            val = next_val;
            for(m=n; m<nsmps; m++) {
                out[m] = val;
            }
            break;
        }
    }
    p->state = state;
    p->t = t;
    p->val = val;
    p->prev_val = prev_val;
    p->next_val = next_val;
    p->segment_end = segment_end;
    p->segment_idx = segment_idx;
    return OK;
}


typedef struct {
    OPDS h;
    MYFLT *out;
    MYFLT *in;
    MYFLT *threshdb;
    MYFLT *atk;
    MYFLT *rel;

    MYFLT patk, prel;
    MYFLT b0_r, a1_r, b0_a, a1_a, level;

} SP_PEAKLIM;

#ifndef dB
/* if below -100dB, set to -100dB to prevent taking log of zero */
#define dB(x) 20.0 * ((x) > 0.00001 ? log10(x) : log10(0.00001))
#endif

#ifndef dB2lin
#define dB2lin(x)    pow( 10.0, (x) / 20.0 )
#endif

static int32_t sp_peaklim_init(CSOUND *csound, SP_PEAKLIM *p) {
    IGN(csound);
    p->a1_r = 0;
    p->b0_r = 1;
    p->a1_a = 0;
    p->b0_a = 1;
    p->patk = -100;
    p->prel = -100;
    p->level = 0;
    if(*p->atk < 0)
        *p->atk = 0.01;
    if(*p->rel < 0)
        *p->rel = 0.1;
    return OK;
}

static int32_t sp_peaklim_compute(CSOUND *csound, SP_PEAKLIM *p) {

    MYFLT *out = p->out;
    SAMPLE_ACCURATE(out);
    MYFLT *in = p->in;

    MYFLT gain = 0;

    MYFLT atk = *p->atk;
    MYFLT rel = *p->rel;

    MYFLT patk = p->patk;
    MYFLT prel = p->prel;
    MYFLT a1_a = p->a1_a;
    MYFLT b0_r = p->b0_r;
    MYFLT a1_r = p->a1_r;
    MYFLT b0_a = p->b0_a;
    MYFLT sr = csound->GetSr(csound);
    MYFLT level = p->level;
    MYFLT insamp;
    MYFLT threshlin = dB2lin(*p->threshdb);

    if(patk != atk) {
        patk = atk;
        a1_a = exp( -1.0 / ( atk * sr ) );
        b0_a = 1 - a1_a;
    }

    if(prel != rel) {
        prel = rel;
        a1_r = exp( -1.0 / ( rel * sr ) );
        b0_r = 1 - a1_r;
    }

    /* change coefficients, if needed */
    n = nsmps - offset;

    while (n--) {
        insamp = *in++;
        MYFLT absinsamp = fabs(insamp);
        if ( absinsamp > level)
            level += b0_a * ( absinsamp - level);
        else
            level += b0_r * ( absinsamp - level);

        gain = min(1.0, threshlin/level);
        *out++ = insamp * gain;
    }
    p->patk = patk;
    p->prel = prel;
    p->a1_a = a1_a;
    p->b0_r = b0_r;
    p->a1_r = a1_r;
    p->b0_a = b0_a;
    p->level = level;
    return OK;
}

// check if a file exists
// taken from https://stackoverflow.com/questions/230062/whats-the-best-way-to-check-if-a-file-exists-in-c


typedef struct {
    OPDS h;
    MYFLT *out;
    STRINGDAT *path;
} FILE_EXISTS;

static int32_t file_exists_init(CSOUND *csound, FILE_EXISTS *p) {
    IGN(csound);
    char *path = p->path->data;
    if(access(path, F_OK) != -1 ) {
        // file exists
        *p->out = 1;
    } else {
        *p->out = 0;
    }
    return OK;
}

// #define USE_LOOKUP_SINE 1


typedef struct {
    OPDS h;
    MYFLT *aout;
    MYFLT *ain;
    MYFLT *kmodfreq;
    MYFLT *kdiodeon;
    MYFLT *kfeedback;
    MYFLT *knonlinearities;
    MYFLT *koversample;

    int started;
    MYFLT r, c1, c2, c3, fgain, bl_c1, bl_c2, bl_c3;
    MYFLT src_f;
    MYFLT sinp, ps_sin_out2;
    MYFLT sin_bl2_1, sin_bl2_2, sin_bl1_1, sin_bl1_2, o_sin_out2;
    MYFLT s1, s2;
    MYFLT ps_fx_out2l;
    MYFLT bl2_1, bl2_2, bl1_1, bl1_2;
    MYFLT o_fx_out2l;
    MYFLT s2l, s1l;
    MYFLT fp_l_os_1, fp_l_os_2, fp_l;
    uint32_t seed;

#ifdef USE_LOOKUP_SINE
    FUNC * ftp;
    MYFLT cpstoinc;
    int32_t  phase;
    int32_t  lomask;
#endif
} t_diode_ringmod;


static int32_t dioderingmod_init(CSOUND *csound, t_diode_ringmod *p) {
    //sim resistance
    p->r = 0.85;

    //fir restoration filter
    p->c1 = 1;
    p->c2 = -0.75;
    p->c3 = 0.17;
    p->fgain = 4;

    //fir bandlimit
    p->bl_c1 = 0.52;
    p->bl_c2 = 0.54;
    p->bl_c3 = -0.02;
    p->started = 0;
    p->seed = csound->GetRandomSeedFromTime();
    p->sinp = 0;
#ifdef USE_LOOKUP_SINE
    MYFLT ifn = -1;
    p->ftp = csound->FTFind(csound, &ifn);
    uint32_t tabsize = p->ftp->flen;
    MYFLT sampledur = 1 / csound->GetSr(csound);
    p->cpstoinc = tabsize * sampledur * 65536;
    p->lomask   = (tabsize - 1) << 3;
    p->phase = 0;
#endif
    return OK;
}


// uniform noise, taken from csoundRand31, returns floats between 0-1
static inline MYFLT randflt(uint32_t *seedptr) {
    uint64_t tmp1;
    uint32_t tmp2;
    /* x = (742938285 * x) % 0x7FFFFFFF */
    tmp1  = (uint64_t) ((int32_t) (*seedptr) * (int64_t) 742938285);
    tmp2  = (uint32_t) tmp1 & (uint32_t) 0x7FFFFFFF;
    tmp2 += (uint32_t) (tmp1 >> 31);
    tmp2  = (tmp2 & (uint32_t) 0x7FFFFFFF) + (tmp2 >> 31);
    (*seedptr) = tmp2;
    return (MYFLT)(tmp2 - 1) / FL(2147483648.0);
}


static inline float
PhaseFrac(uint32_t inPhase) {
    union { uint32_t itemp; float ftemp; } u;
    u.itemp = 0x3F800000 | (0x007FFF80 & ((inPhase)<<7));
    return u.ftemp - 1.f;
}

#define M_PI 3.14159265358979323846
#define pi2 2*M_PI

#define xlobits 14
#define xlobits1 13


static inline MYFLT
lookupi1(const MYFLT* table0, const MYFLT* table1,
         int32_t pphase, int32_t lomask) {
    MYFLT pfrac    = PhaseFrac(pphase);
    uint32_t index = ((pphase >> xlobits1) & lomask);
    MYFLT val1 = *(const MYFLT*)((const char*)table0 + index);
    MYFLT val2 = *(const MYFLT*)((const char*)table1 + index);
    MYFLT out  = val1 + (val2 - val1) * pfrac;
    return out;
}


static int32_t dioderingmod_perf(CSOUND *csound, t_diode_ringmod *p) {
    int os = (int)*p->koversample;
    MYFLT tgt_f = *p->kmodfreq;

    // sx = 16+slider3*1.20103;
    // tgt_f = floor(exp(sx*log(1.059))*8.17742);

    MYFLT fb = *p->kfeedback;
    int diode = (int)*p->kdiodeon;
    MYFLT nl = *p->knonlinearities * 100;
    MYFLT outgain = 1;

    MYFLT *ain = p->ain;
    MYFLT *aout = p->aout;

    if(! p->started) {
        p->src_f = tgt_f;
        p->started = 1;
    }

    MYFLT samplesblock = p->h.insdshead->ksmps;
    MYFLT srate = csound->GetSr(csound);

    //interpolate 'f'
    MYFLT d_f = (tgt_f - p->src_f) / samplesblock;
    MYFLT tf = p->src_f;
    p->src_f = tgt_f;

    int nsmps = p->h.insdshead->ksmps;
    uint32_t seed = p->seed;
    MYFLT pi2_over_srate = pi2 / srate;
    MYFLT sinp = p->sinp;

#ifdef USE_LOOPUP_SINE
    MYFLT *table0 = p->ftp->ftable;
    MYFLT *table1 = table0 + 1;
    int32_t intphase = p->phase;
    MYFLT cpstoinc = p->cpstoinc;
#endif

    for(int i=0; i < nsmps; i++) {
        MYFLT nl_f = nl == 0 ? 0 : randflt(&seed) * 4*nl - 2*nl;
        MYFLT nl_fb = fb == 0 | nl == 0 ? 0 :  (randflt(&seed) * 2*nl - nl)*0.001;

        //interpolate 'f'
        tf += d_f;

        // mod signal gen
#ifdef USE_LOOPUP_SINE
        int32_t phaseinc = (int32_t)(cpstoinc * (tf - nl_f));
        MYFLT sinout = lookupi1(table0, table1, intphase, p->lomask);
        intphase += phaseinc;
#else
        MYFLT sina = (tf - nl_f) * pi2_over_srate;
        // MYFLT sinout = sin(sinp);
        MYFLT sinout = fast_sin(sinp);
        // MYFLT sinout = sinf(sinp);
        sinp += sina;
        if(sinp >= pi2)
            sinp -= pi2;
#endif
        //diode - positive semi-periods
        MYFLT m_out;
        if (diode == 0) {
            m_out = sinout;
        } else {
            //os - diode-ed signal?
            MYFLT d_sin;
            if(os == 0) {
              d_sin = fabs(sinout)*2-0.20260;
            } else {
                //power series in
                MYFLT ps_sin_out1 = 0.5*(sinout + p->ps_sin_out2);
                p->ps_sin_out2 = 0.5 * ps_sin_out1;
                //abs()
                MYFLT ps_d_sin1 = fabs(ps_sin_out1)*2-0.20260;
                MYFLT ps_d_sin2 = fabs(p->ps_sin_out2)*2-0.20260;
                //bandlimit
                MYFLT sin_bl3_1 = p->sin_bl2_1;
                MYFLT sin_bl3_2 = p->sin_bl2_2;
                p->sin_bl2_1 = p->sin_bl1_1;
                p->sin_bl2_2 = p->sin_bl1_2;
                p->sin_bl1_1 = ps_d_sin1;
                p->sin_bl1_2 = ps_d_sin2;
                MYFLT sin_bl_out1 = (p->sin_bl1_1*p->bl_c1 + p->sin_bl2_1*p->bl_c2 + sin_bl3_1*p->bl_c3);
                MYFLT sin_bl_out2 = (p->sin_bl1_2*p->bl_c1 + p->sin_bl2_2*p->bl_c2 + sin_bl3_2*p->bl_c3);
                //power series out
                MYFLT o_sin_out1 = 0.5*(sin_bl_out1+p->o_sin_out2);
                p->o_sin_out2 = 0.5*(sin_bl_out2+o_sin_out1);
                //fir restoration
                MYFLT s3 = p->s2;
                p->s2 = p->s1;
                p->s1 = o_sin_out1;
                d_sin = (p->s1*p->c1+p->s2*p->c2+s3*p->c3) * p->fgain;
            }
            m_out = d_sin;
        }

        //-------------------------------------------------
        //input
        //-------------------------------------------------
        MYFLT in_l = ain[i];
        MYFLT fx_outl;
        if(os == 0) {   // No oversampling
            //feedback ala Paul Kellet
            p->fp_l = (in_l+(fb-nl_fb)*p->fp_l)*sinout*p->r;
            //multiply carrier with mod
            MYFLT s_out_l = m_out*in_l;
            //apply feedback
            s_out_l += p->fp_l;

            fx_outl = s_out_l;


        } else {      // Yes oversampling
            //power series in
            MYFLT ps_fx_out1l = 0.5*(in_l+p->ps_fx_out2l);
            p->ps_fx_out2l = 0.5*ps_fx_out1l;

            //------------------------
            //fx
            //------------------------
            p->fp_l_os_1 = (ps_fx_out1l+(fb-nl_fb)*p->fp_l_os_1)*sinout*p->r;
            MYFLT s_out_l_os_1 = m_out*ps_fx_out1l;
            s_out_l_os_1 += p->fp_l_os_1;

            p->fp_l_os_2 = (p->ps_fx_out2l+(fb-nl_fb)*p->fp_l_os_2)*sinout*p->r;
            MYFLT s_out_l_os_2 = m_out*p->ps_fx_out2l;
            s_out_l_os_2 += p->fp_l_os_2;

            //------------------------
            //bandlimit
            //------------------------
            MYFLT bl3_1 = p->bl2_1;
            MYFLT bl3_2 = p->bl2_2;

            p->bl2_1 = p->bl1_1;
            p->bl2_2 = p->bl1_2;

            p->bl1_1 = s_out_l_os_1;
            p->bl1_2 = s_out_l_os_2;

            MYFLT bl_out1 = (p->bl1_1*p->bl_c1 + p->bl2_1*p->bl_c2 + bl3_1*p->bl_c3);
            MYFLT bl_out2 = (p->bl1_2*p->bl_c1 + p->bl2_2*p->bl_c2 + bl3_2*p->bl_c3);

            //------------------------
            //power series out
            //------------------------
            MYFLT o_fx_out1l = 0.5*(bl_out1+p->o_fx_out2l);
            p->o_fx_out2l = 0.5*(bl_out2+o_fx_out1l);

            //fir restoration
            MYFLT s3l = p->s2l;
            p->s2l = p->s1l;
            p->s1l = o_fx_out1l;

            fx_outl = (p->s1l*p->c1+p->s2l*p->c2+s3l*p->c3)*p->fgain;
        }

        aout[i] = fx_outl * p->r * outgain;
    }
    p->seed = seed;
    p->sinp = sinp;
#ifdef USE_LOOPUP_SINE
    p->phase = intphase;
#endif
    return OK;
}


/**
 * pread / pwrite
 *
 * Communicate between notes via p-fields
 *
 */

INSTRTXT *find_instrdef(INSDS *insdshead, int instrnum) {
    INSDS *instr = insdshead->nxtact;
    while (instr) {
        if(instr->insno == instrnum)
            return instr->instr;
        instr = instr->nxtact;
    }
    // not found yet, search backwards
    instr = insdshead->prvact;
    while (instr) {
        if(instr->insno == instrnum)
            return instr->instr;
        instr = instr->prvact;
    }
    return NULL;
}

INSDS *find_instance_exact(INSTRTXT *instrdef, MYFLT instrnum) {
    INSDS *instance = instrdef->instance;
    while(instance) {
        if (instance->actflg && instance->p1.value == instrnum) {
            return instance;
        }
        instance = instance->nxtinstance;
    }
    return NULL;
}


/**
 * pread
 *
 * read p-fields from a different instrument
 *
 * iout pread instrnum, index  [, inotfound=-1]
 * kout pread instrnum, index  [, inotfound=-1]
 * kout pread instrnum, kindex [, inotfound=-1]
 *
 */

typedef struct {
    OPDS h;
    MYFLT *outval, *instrnum, *pindex, *inotfound;

    CS_VAR_MEM *pfields;    // points to instr's pfields
    int maxpfield;         // max readable pfield
    INSDS *instr;         // found instr
    int retry;            // should we retry when not found?
    int found;            // -1 if not yet searched, 0 if not found, 1 if found
    INSTRTXT *instrtxt;   // instrument definition
} PREAD;


int32_t pread_search(CSOUND *csound, PREAD *p) {
    IGN(csound);
    MYFLT p1 = *p->instrnum;
    INSDS *instr;
    p->found = 0;
    if(!p->instrtxt)
        p->instrtxt = csound->GetInstrument(csound, (int)p1, NULL);
    if(!p->instrtxt)
        return 0;
    // found an instrument definition
    p->maxpfield = p->instrtxt->pmax;

    if(p1 != floor(p1)) {
        // fractional instrnum
        instr = find_instance_exact(p->instrtxt, p1);
    } else {
        // find first instance of this instr
        instr = p->instrtxt->instance;
        while(instr) {
            if(instr->actflg)
                break;
            instr = instr->nxtinstance;
        }
    }
    if(!instr)
        return 0;
    p->found = 1;
    p->instr = instr;
    p->pfields = &(instr->p0);
    return 1;
}

static int32_t
pread_init(CSOUND *csound, PREAD *p) {
    IGN(csound);
    MYFLT p1 = *p->instrnum;
    p->found = -1;
    // a negative instr. number indicates NOT to search again after failed to
    // find the given instance
    if(p1 < 0) {
        p->retry = 0;
        *p->instrnum = p1 = -p1;
    } else {
        p->retry = 1;
    }
    *p->outval = *p->inotfound;
    return OK;
}

static int32_t
pread_perf(CSOUND *csound, PREAD *p) {
    int idx = (int)*p->pindex;
    if(p->found == -1 || (p->found==0 && p->retry)) {
        int found = pread_search(csound, p);
        if (!found) {
            return OK;
        }
    }
    if(!p->instr->actflg) {
        return OK;
    }
    if(idx > p->maxpfield) {
        return PERFERRF(Str("pread: can't read p%d (max index = %d)"), idx, p->maxpfield);
    }
    MYFLT value = p->pfields[idx].value;
    *p->outval = value;
    return OK;
}

static int32_t
pread_i(CSOUND *csound, PREAD *p) {
    int ans = pread_init(csound, p);
    if(ans == NOTOK)
        return NOTOK;
    return pread_perf(csound, p);
}

#define PWRITE_MAXINPUTS 40


/**
 * pwrite
 *
 * Modifies the p-fields of another instrument
 *
 * pwrite instrnum, index1, value1 [, index2, value2, ...]
 *
 * instrnum (i): the instrument number.
 *      * If a fractional number is given, only the instance with this
 *        exact number is modified.
 *      * If an integer number is given, ALL instances of this instrument are modified
 *      * Matching instances are searched at performance, and search is retried
 *        if no matching instances were found. If a negative instrnum is given,
 *        search is attempted only once and the opcode becomes a NOOP if no instances
 *        were found
 */

typedef struct {
    OPDS h;
    // inputs
    MYFLT *instrnum;
    MYFLT *inputs[PWRITE_MAXINPUTS];

    MYFLT p1;             // cached instrnum, will always be positive
    int numpairs;         // number of index:value pairs
    int retry;            // should we retry when no instance matched?
    INSDS *instr;         // a pointer to the found instance, when matching exact number
    INSTRTXT *instrtxt;   // used in broadcasting mode, a pointer to the instrument template
    int maxpfield;        // max pfield accepted by the instrument
    int broadcasting;     // are we broadcasting?
    int found;            // have we found any matching instances?
    CS_VAR_MEM *pfields;  // a pointer to the insance pfield, when doing exact matching
} PWRITE;


static inline void
pwrite_writevalues(CSOUND *csound, PWRITE *p, CS_VAR_MEM *pfields) {
    for(int pair=0; pair < p->numpairs; pair++) {
        int indx = (int)*(p->inputs[pair*2]);
        MYFLT value = *(p->inputs[pair*2+1]);
        if(indx > p->maxpfield)
            MSGF("pwrite: can't write to p%d (max index=%d)\n", indx, p->maxpfield);
        else
            pfields[indx].value = value;
    }
}


static int32_t
pwrite_search(CSOUND *csound, PWRITE *p) {
    IGN(csound);
    MYFLT p1 = p->p1;
    if(p->instrtxt == NULL) {
        // INSTRTXT *instrdef = find_instrdef(p->h.insdshead, (int)p1);
        INSTRTXT *instrdef = csound->GetInstrument(csound, (int)p1, NULL);
        if(!instrdef) {
            return 0;
        }
        p->instrtxt = instrdef;
        p->maxpfield = instrdef->pmax;
    }
    // if we are not broadcasting, search the exact match
    if (!(p->broadcasting)) {
        INSDS *instr = find_instance_exact(p->instrtxt, p1);
        if(!instr) {
            return 0;
        }
        p->instr = instr;
        p->pfields = &(instr->p0);
    }
    return 1;
}


static int32_t
pwrite_initcommon(CSOUND *csound, PWRITE *p) {
    IGN(csound);
    MYFLT p1 = *p->instrnum;
    if (p1 < 0) {
        p1 = -p1;
        p->retry = 0;
    } else {
        p->retry = 1;
    }
    p->p1 = p1;
    p->broadcasting = floor(p1) == p1;
    p->numpairs = (csound->GetInputArgCnt(p) - 1) / 2;
    p->found = -1;
    p->instrtxt = NULL;
    return OK;
}


static int32_t
pwrite_perf(CSOUND *csound, PWRITE *p) {
    if(p->found == -1 || (p->found == 0 && p->retry))
        p->found = pwrite_search(csound, p);
    if(!p->found)
        return OK;

    if (!p->broadcasting) {
        pwrite_writevalues(csound, p, p->pfields);
        return OK;
    }
    // broadcasting mode
    INSDS *instance = p->instrtxt->instance;
    while(instance) {
        if (instance->actflg) {  // is this instance active?
            pwrite_writevalues(csound, p, &(instance->p0));
        }
        instance = instance->nxtinstance;
    }
    return OK;
}


static int32_t
pwrite_i(CSOUND *csound, PWRITE *p) {
    int32_t ans = pwrite_initcommon(csound, p);
    if(ans == NOTOK) return NOTOK;
    return pwrite_perf(csound, p);
}


/** uniqinstance
 *
 * given an integer instrument number, return a fractional instr. number
 * which is not active now and can be used as p1 for "event" or similar
 * opcodes to create a unique instance of an instrument
 *
 * instrnum  uniqinstrance integer_instrnum
 *
 */

#define UNIQ_NUMSLOTS 10000

typedef struct {
    OPDS h;
    MYFLT *out, *int_instrnum, *max_instances;
    int p1;
    int numslots;
    char slots[UNIQ_NUMSLOTS];
} UNIQINSTANCE;

static MYFLT
uniqueinstance_(CSOUND *csound, UNIQINSTANCE *p) {
    // char slots[UNIQ_NUMSLOTS] = {0};
    char *slots = p->slots;
    int numslots = p->numslots;
    memset(slots, 0, numslots);
    int p1 = p->p1;
    int idx;
    int maxidx = 0;
    int minidx = numslots;
    MYFLT fractional_part, integral_part;

    INSTRTXT *instrtxt = csound->GetInstrument(csound, p1, NULL);
    if(!instrtxt || instrtxt->instance == NULL) {
        // no instances of this instrument, so pick first index
        return p1 + FL(1)/numslots;
    }

    INSDS *instance = instrtxt->instance;
    while(instance) {
        if(instance->actflg && instance->p1.value != instance->insno) {
            fractional_part = modf(instance->p1.value, &integral_part);
            idx = (int)(fractional_part * numslots + 0.5);
            if(idx > maxidx)
                maxidx = idx;
            else if(idx < minidx)
                minidx = idx;
            slots[idx] = 1;
        }
        instance = instance->nxtinstance;
    }
    if(maxidx + 1 < numslots) {
        return p1 + (maxidx+1)/FL(numslots);
    }
    if(minidx > 2) {
        return p1 + (minidx-1)/FL(numslots);
    }
    for(int i=1; i < numslots; i++) {
        if(slots[i] == 0) {
            return p1 + i / FL(numslots);
        }
    }
    return -1;
}

static int32_t
uniqueinstance_dealloc(CSOUND *csound, UNIQINSTANCE *p) {
    csound->Free(csound, p->slots);
    return OK;
}

static int32_t
uniqueinstance_initcommon(CSOUND *csound, UNIQINSTANCE *p) {
    IGN(csound);
    p->numslots = (int)*p->max_instances;
    if(p->numslots == 0)
        p->numslots = UNIQ_NUMSLOTS;
    else if(p->numslots > UNIQ_NUMSLOTS)
        p->numslots = UNIQ_NUMSLOTS;
    // p->slots = csound->Malloc(csound, p->numslots * sizeof(char));
    // register_deinit(csound, p, uniqueinstance_dealloc);
    MYFLT instrnum = uniqueinstance_(csound, p);

    *p->out = instrnum;
    return OK;
}

static int32_t
uniqueinstance_i(CSOUND *csound, UNIQINSTANCE *p) {
    p->p1 = (int)*p->int_instrnum;
    return uniqueinstance_initcommon(csound, p);
}

static int32_t
uniqueinstance_S_init(CSOUND *csound, UNIQINSTANCE *p) {
    STRINGDAT *instrname = (STRINGDAT *)p->int_instrnum;
    p->p1 = csound->strarg2insno(csound, instrname->data, 1);
    if (UNLIKELY(p->p1 == NOT_AN_INSTRUMENT))
        return NOTOK;
    return uniqueinstance_initcommon(csound, p);
}


/*

instr 100  ; generator
    pset 0,0,0, 440, 0.5, 4000
    kfreq, kamp, kcutoff passign 4
    a0 ... ; do something with this params
endin

instr 200  ; controls
    inst uniqinstance 100
    event_i inst, 0, -1
    kfreq line 440, p3, 880
    kcutoff line 4000, p3, 400
    pwrite inst, 4, kfreq, 6, kcutoff
endin

 */


/** ----------------- atstop ---------------

  schedule an instrument when the note stops

  NB: this is scheduled not at release start, but when the
  note is deallocated

  atstop Sintr, idelay, idur, pfields...
  atstop instrnum, idelay, idur, pfields...

*/

#define MAXPARGS 31

typedef struct {
    OPDS h;
    // outputs

    // inputs
    void *instr;
    MYFLT *pargs [MAXPARGS];

    // internal
    MYFLT instrnum;   // cached instrnum

} SCHED_DEINIT;


static int32_t
atstop_deinit(CSOUND *csound, SCHED_DEINIT *p) {
    EVTBLK evt;
    memset(&evt, 0, sizeof(EVTBLK));
    evt.opcod = 'i';
    evt.strarg = NULL;
    evt.scnt = 0;
    evt.pinstance = NULL;
    evt.p2orig = *p->pargs[0];
    evt.p3orig = *p->pargs[1];
    uint32_t pcnt = max(3, p->INOCOUNT);
    evt.p[1] = p->instrnum;
    for(uint32_t i=0; i < pcnt-1; i++) {
        evt.p[2+i] = *p->pargs[i];
    }
    evt.pcnt = (int16_t) pcnt;
    csound->insert_score_event_at_sample(csound, &evt,
                                         csound->GetCurrentTimeSamples(csound));
    return OK;
}


static int32_t
atstop_(CSOUND *csound, SCHED_DEINIT *p, MYFLT instrnum) {
    p->instrnum = instrnum;
    register_deinit(csound, p, atstop_deinit);
    return OK;
}

static int32_t
atstop_i(CSOUND *csound, SCHED_DEINIT *p) {
    MYFLT instrnum = *((MYFLT*)p->instr);
    return atstop_(csound, p, instrnum);
}

static int32_t
atstop_s(CSOUND *csound, SCHED_DEINIT *p) {
    STRINGDAT *instrname = (STRINGDAT*) p->instr;
    int32_t instrnum = csound->strarg2insno(csound, instrname->data, 1);
    if (UNLIKELY(instrnum == NOT_AN_INSTRUMENT)) return NOTOK;
    return atstop_(csound, p, (MYFLT) instrnum);
}

/*
 * accum
 *
 * a simple accumulator
 *
 * kout accum kstep, initial=0
 *
 * Can be used together with changed for all opcodes which need an increasing
 * trigger (printf, for example).
 *
 * printf "kvar=%f \n", accum(changed(kvar)), kvar
 *
 */
typedef struct {
    OPDS h;
    MYFLT *out;
    MYFLT *step, *initial_value;
    MYFLT value;
} ACCUM;

static int32_t
accum_init(CSOUND *csound, ACCUM *p) {
    p->value = *p->initial_value;
    return OK;
}

static int32_t
accum_perf(CSOUND *csound, ACCUM *p) {
    *p->out = p->value;
    p->value += *p->step;
    return OK;
}

static int32_t
accum_perf_audio(CSOUND *csound, ACCUM *p) {
    MYFLT step = *p->step;
    MYFLT value = p->value;
    MYFLT *out = p->out;

    SAMPLE_ACCURATE(out);

    for(int n=offset; n<nsmps; n++) {
        out[n] = value;
        value += step;
    }
    p->value = value;
    return OK;
}

typedef struct {
    OPDS h;
    MYFLT *out;
    MYFLT *in1;
    MYFLT *in2;
} FUNC12;

/** frac2int
 *
 * convert the fractional part to an integer by extracting
 * the fractional part and multiplying by a factor
 *
 * thourght to be used together with fractional p1
 *
 * schedule 10 + inum/1000, ...
 *
 * in instr 10
 * inum = frac2int(p1, 1000)  ; this should result in the original inum if the same factor
 *                            ; (in this case 1000) is used
 *
 * NB: the integral part is discarded
 *
 * The inverse is not necessary, as it is simply inum/ifactor
 */

// frac2int(10.456, 1000) -> 456
// frac2int(0.134, 100)   -> 13


static int32_t
frac2int(CSOUND *csound, FUNC12 *p) {
    IGN(csound);
    double integ;
    double fract = modf(*p->in1, &integ);
    MYFLT mul = *p->in2;
    *p->out = floor(fract * mul + 0.5);
    return OK;
}


// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

/*

  iout[]  vecview ifn, istart, iend

  An array view into a function table. The returned array does not
  own the memory, it is just a view (an alias) into the memory
  allocated by the function table

 */

// init array 'arr' to point to data


static void _init_array_view(CSOUND *csound, ARRAYDAT *outarr, MYFLT *sourcedata,
                             int sourcesize, size_t allocated) {
    if(outarr->data != NULL) {
        printf("$$$ freeing original data (size=%d, allocated=%ld) \n",
               outarr->sizes[0], outarr->allocated);
        csound->Free(csound, outarr->data);
    } else {
        CS_VARIABLE* var = outarr->arrayType->createVariable(csound, NULL);
        outarr->arrayMemberSize = var->memBlockSize;
    }
    size_t minallocated = outarr->arrayMemberSize * sourcesize;
    outarr->allocated = minallocated > allocated ? minallocated : allocated;
    outarr->data = sourcedata;
    outarr->dimensions = 1;
    if(outarr->sizes == NULL)
        outarr->sizes = csound->Malloc(csound, sizeof(int));
    outarr->sizes[0] = sourcesize;
    // _mark_array_as_view(csound, outarr);
}


// arrsetslice in, start=0, end=0, step=1
// arrsetslice in, start=0, end=0
typedef struct {
    OPDS h;
    ARRAYDAT *in;
    MYFLT *value, *start, *end, *step;
} ARRSETSLICE;

static int32_t
array_set_slice(CSOUND *csound, ARRSETSLICE *p) {
    if(p->in->dimensions!=1) {
        MSGF("Expected array of 1 dimension, but array has"
             "got %d dimensions", p->in->dimensions);
        return NOTOK;
    }
    int32_t start = (int32_t)*p->start;
    int32_t end = (int32_t)*p->end;
    int32_t step = (int32_t)*p->step;
    MYFLT value = *p->value;
    if(end == 0) {
        end = p->in->sizes[0];
    }
    MYFLT *data = p->in->data;
    for(int i=start; i<end; i+=step) {
        data[i] = value;
    }
    return OK;
}


typedef struct {
    OPDS h;
    ARRAYDAT *out;
    MYFLT *ifn, *istart, *iend;
    FUNC * ftp;
    int size;
} TABALIAS;

static int
        tabalias_deinit(CSOUND *csound, TABALIAS *p) {
        IGN(csound);
        p->out->data = NULL;
        p->out->sizes[0] = 0;
        return OK;
    }

    static int
        tabalias_init(CSOUND *csound, TABALIAS *p) {
        FUNC *ftp;
        ftp = csound->FTnp2Find(csound, p->ifn);
        if (UNLIKELY(ftp == NULL))
            return NOTOK;
        int tabsize = ftp->flen;
        int start = (int)*p->istart;
        int end   = (int)*p->iend;
        if(end == 0)
            end = tabsize;
        p->size = end - start;
        p->ftp = ftp;

        if (tabsize < end)
            return INITERR("end is bigger than the length of the table");

        _init_array_view(csound, p->out, &(ftp->ftable[start]), end-start,
                         sizeof(MYFLT)*(tabsize-start));
        register_deinit(csound, p, tabalias_deinit);
        return OK;
    }


/*

  iout[]  vecview  in[],  istart, iend=0
  kout[]  vecview  kin[], istart, iend=0

  An array view into another array, useful to operate on a row
  of a large 2D array.

*/


    typedef struct {
        OPDS h;
        ARRAYDAT *out, *in;
        MYFLT *istart, *iend;
        MYFLT *dataptr;
        int size;
    } ARRAYVIEW;

    static void _array_view_deinit(ARRAYDAT *arr) {
        arr->data = NULL;
        arr->sizes[0] = 0;
    }


    static int
        arrayview_deinit(CSOUND *csound, ARRAYVIEW *p) {
        IGN(csound);
        _array_view_deinit(p->out);
        return OK;
    }


    static int32_t
        arrayview_init(CSOUND *csound, ARRAYVIEW *p) {
        if(p->in->data == NULL)
            return INITERR("source array has not been initialized");
        if(p->in->dimensions > 1)
            return INITERR(Str("A view can only be taken from a 1D array"));

        int end   = (int)*p->iend;
        int start = (int)*p->istart;
        if(end == 0)
            end = p->in->sizes[0];
        _init_array_view(csound, p->out, &(p->in->data[start]), end - start,
                         p->in->allocated - start);
        register_deinit(csound, p, arrayview_deinit);
        return OK;
    }


// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
/*
 * ref
 *
 * iref ref iArray
 * iArray deref iref
 *
 * iref ref kscalar
 * kscalar deref iref
 *
 */


    enum RefType { RefScalar, RefAudio, RefArrayScalar };


    typedef struct {
        int active;
        MYFLT *data;
        enum RefType type;
        int size;
        size_t allocated;
        int refcount;
        int forward_refs;
        int ownsdata;
        int isglobal;
    } REF_HANDLE;

    typedef struct {
        CSOUND *csound;
        int numhandles;
        REF_HANDLE *handles;
        intarray_t slots;
    } REF_GLOBALS;

// static int32_t ref_reset(CSOUND *csound, REF_GLOBALS *g);

#define REF_GLOBALS_VARNAME "__ref_globals__"

    static REF_GLOBALS *ref_globals(CSOUND *csound) {
        REF_GLOBALS *g = csound->QueryGlobalVariable(csound, REF_GLOBALS_VARNAME);
        if(g != NULL) return g;

        int err = csound->CreateGlobalVariable(csound, REF_GLOBALS_VARNAME, sizeof(REF_GLOBALS));
        if(err != 0) {
            INITERR("failed to create globals for ref");
            return NULL;
        }
        g = csound->QueryGlobalVariable(csound, REF_GLOBALS_VARNAME);
        g->csound = csound;
        g->handles = csound->Calloc(csound, sizeof(REF_HANDLE) * 8);
        g->numhandles = 64;
        intpool_init(csound, &(g->slots), g->numhandles);
        // csound->RegisterResetCallback(csound, (void *)g, (int32_t(*)(CSOUND*, void*))ref_reset);
        return g;
    }

    static inline void _ref_handle_init(REF_HANDLE *h) {
        h->refcount = 0;
        h->forward_refs = 0;
        h->ownsdata = 0;
        h->size = 0;
        h->allocated = 0;
        h->data = NULL;
    }

    static void _ref_handle_release(CSOUND *csound, REF_HANDLE *h) {
        if(h->data != NULL && h->ownsdata) {
            csound->Free(csound, h->data);
            h->data = NULL;
            if(csound->GetDebug(csound)) {
                MSG(">>>> ref: Releasing memory of array ref \n");
            }
        }
        _ref_handle_init(h);
    }

    static inline int32_t
        _ref_get_slot(REF_GLOBALS *g) {
        CSOUND *csound = g->csound;
        int freeslot = intpool_pop(csound, &(g->slots));
        if(g->slots.capacity > g->numhandles) {
            // if we are out of slots the slot pool is resized. If it is bigger
            // that the number of handles, we need to match the handles array
            g->handles = csound->ReAlloc(csound, g->handles, sizeof(REF_HANDLE)*g->slots.capacity);
            g->numhandles = g->slots.capacity;
        }
        REF_HANDLE *h = &(g->handles[freeslot]);
    if(h->active == 1) {
        printf("Got free slot %d, but handle is active???\n", freeslot);
        return -1;
    }
    _ref_handle_init(h);
    return freeslot;
}

static inline int32_t
_ref_get_slot2(REF_GLOBALS *g) {
    REF_HANDLE *handles = g->handles;
    for(int i=0; i<g->numhandles; i++) {
        REF_HANDLE *h = &(handles[i]);
        if(h->active == 0 && h->ownsdata == 0) {
            _ref_handle_init(h);
            return i;
        }
    }
    // no free slots, time to grow
    int idx = g->numhandles;
    CSOUND *csound = g->csound;
    g->numhandles *= 2;
    g->handles = csound->ReAlloc(csound, g->handles, sizeof(REF_HANDLE)*g->numhandles);
    _ref_handle_init(&(g->handles[idx]));
    return idx;
}


typedef struct {
    OPDS h;
    MYFLT *idx;
    ARRAYDAT *arr;
    MYFLT *increfs;
    int slot;
    REF_GLOBALS *g;
} REF_NEW_ARRAY;


static int32_t ref_new_deinit(CSOUND *csound, REF_NEW_ARRAY *p);

static int32_t
ref_new_array(CSOUND *csound, REF_NEW_ARRAY *p) {
    if(p->arr->data == NULL || p->arr->allocated == 0) {
        return INITERR("Cannot take a reference from uninitialized array");
    }
    if(p->arr->dimensions != 1) {
        return INITERRF("Only 1D arrays supported (array has %d dims)", p->arr->dimensions);
    }
    REF_GLOBALS *g = ref_globals(csound);
    int slot = _ref_get_slot(g);
    REF_HANDLE *h = &(g->handles[slot]);
    _ref_handle_init(h);
    char *argname = csound->GetInputArgName(p, 0);
    h->isglobal = argname[0] == 'g' ? 1 : 0;
    h->active = 1;
    h->type = RefArrayScalar;  // todo: detect type
    h->data = p->arr->data;
    h->size = p->arr->sizes[0];
    h->allocated = p->arr->allocated;
    h->ownsdata = 0;
    h->refcount = 1;
    h->forward_refs = (int)*p->increfs;
    p->g = g;
    *p->idx = slot;
    p->slot = slot;
    register_deinit(csound, p, ref_new_deinit);
    return OK;
}


void _handle_copy_data_from_array(CSOUND *csound, ARRAYDAT *arr, REF_HANDLE *h) {
    size_t numbytes = arr->arrayMemberSize * arr->sizes[0];
    h->data = csound->Malloc(csound, numbytes);
    memcpy(h->data, arr->data, numbytes);
    h->size = arr->sizes[0];
    h->allocated = numbytes;
    h->ownsdata = 1;
}



static int32_t ref_handle_decref(CSOUND *csound, REF_HANDLE *h) {
    if(h->refcount <= 0) {
        return INITERRF("Cannot decrease refcount (refcount now: %d)", h->refcount);
    }
    h->refcount--;
    if(h->refcount == 0 && h->forward_refs == 0) {
        _ref_handle_release(csound, h);
    }
    return OK;
}


static int32_t
ref_new_deinit(CSOUND *csound, REF_NEW_ARRAY *p) {
    REF_HANDLE *h = &(p->g->handles[p->slot]);
    h->active = 0;
    // if there are clients of this data, we should own the
    // data (if we don't already own it or it is global)
    h->refcount--;
    if(h->refcount < 0) {
        return PERFERRF("Error deiniting ref: refcount was %d", h->refcount);
    }
    if(h->refcount > 0 || h->forward_refs > 0) {
        // there are clients
        if(!h->isglobal && !h->ownsdata) {
            _handle_copy_data_from_array(csound, p->arr, h);
        }
    } else {
        // no clients
        _ref_handle_release(csound, h);
    }
    return OK;
}

typedef struct {
    OPDS h;
    ARRAYDAT *arr;
    MYFLT *idx;
    MYFLT *catch_forward_ref;
    REF_HANDLE *handle;
} DEREF_ARRAY;




static int32_t
deref_array_deinit(CSOUND *csound, DEREF_ARRAY *p) {
    ref_handle_decref(csound, p->handle);
    p->arr->data = NULL;
    p->arr->sizes[0] = 0;
    p->arr->dimensions = 0;
    return OK;
}

static int32_t
deref_array(CSOUND *csound, DEREF_ARRAY *p) {
    int slot = (int)*p->idx;
    REF_GLOBALS *g = ref_globals(csound);
    REF_HANDLE *h = &(g->handles[slot]);
    if(h->data == NULL)
        return INITERR("Handle not active");
    if(h->allocated == 0)
        return INITERR("Array has no elements allocated");
    int forward_refs = (int)*p->catch_forward_ref;
    if(forward_refs > 0) {
        if(h->forward_refs == 0) {
            if(csound->GetDebug(csound))
                MSG("Tried to decref a forward reference, but handle has no "
                    "references left\n");
        } else {
            h->forward_refs--;
        }
    }
    _init_array_view(csound, p->arr, h->data, h->size, h->allocated);
    h->refcount++;
    p->handle = h;

    register_deinit(csound, p, deref_array_deinit);
    return OK;
}

static int32_t ref_handle_valid(REF_GLOBALS *g, int slot) {
    // array out of bounds
    if(slot < 0 || slot >= g->numhandles)
        return 0;
    REF_HANDLE *h = &(g->handles[slot]);
    // an active handle is always valid
    if(h->active == 1)
        return 1;
    // an inactive array handle can also be valid as a deref if it owns data
    if(h->type == RefArrayScalar && h->ownsdata)
        return 1;
    return 0;
}

typedef struct {
    OPDS h;
    MYFLT *args[10];
    REF_GLOBALS *g;
    REF_HANDLE *handle;
    int numouts;
} REF1;

#define RETURN_IF_NOTOK(code) { int32_t _ret = (code); if(_ret != OK) return _ret; }


static inline void
ref1_init(CSOUND *csound, REF1 *p) {
    p->g = ref_globals(csound);
    p->numouts = csound->GetOutputArgCnt(p);
}

static int32_t
ref1_gethandle(CSOUND *csound, REF1 *p) {
    ref1_init(csound, p);
    int slot = (int)*p->args[p->numouts];
    if(!ref_handle_valid(p->g, slot)) {
        return INITERRF("Handle %d not valid", slot);
    }
    p->handle = &(p->g->handles[slot]);
    return OK;
}

static int32_t
ref1_gethandle_perf(CSOUND *csound, REF1 *p) {
    ref1_init(csound, p);
    int slot = (int)*p->args[p->numouts];
    if(!ref_handle_valid(p->g, slot)) {
        return PERFERRF("Handle %d not valid", slot);
    }
    p->handle = &(p->g->handles[slot]);
    return OK;
}


static int32_t
ref_valid_perf(CSOUND *csound, REF1 *p) {
    IGN(csound);
    int slot = (int)*p->args[1];
    *p->args[0] = ref_handle_valid(p->g, slot);
    return OK;
}

static int32_t
ref_valid_i(CSOUND *csound, REF1 *p) {
    ref1_init(csound, p);
    return ref_valid_perf(csound, p);
}


static int32_t
ref_scalar_deinit(CSOUND *csound, REF1 *p) {
    if(p->handle->refcount <= 0)
        return PERFERRF("Tried to deinit ref, but refcount: %d", p->handle->refcount);
    p->handle->refcount--;
    if(p->handle->refcount == 0)
        _ref_handle_release(csound, p->handle);
    return OK;
}

static int32_t
ref_scalar_init(CSOUND *csound, REF1 *p) {
    REF_GLOBALS *g = ref_globals(csound);
    p->g = g;
    int slot = _ref_get_slot(g);
    REF_HANDLE *h = &(g->handles[slot]);
    _ref_handle_init(h);
    h->isglobal = csound->GetInputArgName(p, 0)[0] == 'g' ? 1 : 0;
    h->active = 1;
    h->type = RefScalar;
    h->data = csound->Malloc(csound, sizeof(MYFLT));
    h->size = 1;
    h->allocated = 1*sizeof(MYFLT);
    h->refcount = 1;
    h->ownsdata = 1;
    h->data[0] = *(p->args[1]);

    p->handle = h;
    register_deinit(csound, p, ref_scalar_deinit);
    return OK;
}

static int32_t
ref_scalar_perf(CSOUND *csound, REF1 *p) {
    IGN(csound);
    p->handle->data[0] = *(p->args[1]);
    return OK;
}

// kval deref iref
static int32_t
deref_scalar_init(CSOUND *csound, REF1 *p) {
    ref1_init(csound, p);
    int slot = (int)*p->args[1];
    if(!ref_handle_valid(p->g, slot)) {
        return INITERR("Handle is not valid");
    }
    REF_HANDLE *h = &(p->g->handles[slot]);
    if(h->type != RefScalar)
        return INITERRF("Expected a ref of scalar type (got %d)", h->type);
    p->handle = h;
    h->refcount++;
    *p->args[0] = h->data[0];
    register_deinit(csound, p, ref_scalar_deinit);
    return OK;
}

static int32_t
deref_scalar_perf(CSOUND *csound, REF1 *p) {
    IGN(csound);
    *p->args[0] = p->handle->data[0];
    return OK;
}

static int32_t
ref_audio_init(CSOUND *csound, REF1 *p) {
    REF_GLOBALS *g = ref_globals(csound);
    p->g = g;
    int slot = _ref_get_slot(g);
    REF_HANDLE *h = &(g->handles[slot]);
    h->isglobal = csound->GetInputArgName(p, 0)[0] == 'g' ? 1 : 0;
    h->active = 1;
    h->type = RefAudio;
    h->size = p->h.insdshead->ksmps;
    h->allocated = h->size * sizeof(MYFLT);
    h->data = csound->Malloc(csound, h->allocated);
    p->handle = h;
    h->refcount = 1;
    h->ownsdata = 1;
    register_deinit(csound, p, ref_scalar_deinit);
    return OK;
}

static int32_t
ref_audio_perf(CSOUND *csound, REF1 *p) {
    IGN(csound);
    int ksmps = p->h.insdshead->ksmps;
    MYFLT *in = p->args[1];
    MYFLT *out = p->handle->data;
    memcpy(out, in, ksmps*sizeof(MYFLT));
    return OK;
}

static int32_t
deref_audio_init(CSOUND *csound, REF1 *p) {
    REF_GLOBALS *g = ref_globals(csound);
    p->g = g;
    int slot = (int)*p->args[1];
    if(!ref_handle_valid(p->g, slot)) {
        return INITERR("Handle is not valid");
    }
    REF_HANDLE *h = &(p->g->handles[slot]);
    if(h->type != RefAudio)
        return INITERRF("Expected a ref of scalar type (got %d)", h->type);
    p->handle = h;
    h->refcount++;
    register_deinit(csound, p, ref_scalar_deinit);
    return OK;
}

static int32_t
deref_audio_perf(CSOUND *csound, REF1 *p) {
    IGN(csound);
    int ksmps = p->h.insdshead->ksmps;
    MYFLT *in = p->handle->data;
    MYFLT *out = p->args[0];
    memcpy(out, in, ksmps*sizeof(MYFLT));
    return OK;
}


static int32_t
ref_inc_i(CSOUND *csound, REF1 *p) {
    ref1_gethandle(csound, p);
    p->handle->refcount++;
    return OK;
}

static int32_t
ref_inc_perf(CSOUND *csound, REF1 *p) {
    RETURN_IF_NOTOK(ref1_gethandle_perf(csound, p));
    p->handle->refcount++;
    return OK;
}

/* xtracycles
 *
 * inumcycles xtracycles
 *
 * Returns the number of extra cycles (extended through opcodes)
 * like xtratim or through xtratim directly
 */

typedef struct {
    OPDS h;
    MYFLT *out;
    MYFLT state1;
} OPCk_0;

static int32_t
xtracycles(CSOUND *csound, OPCk_0 *p) {
    IGN(csound);
    int numcycles = p->h.insdshead->xtratim;
    *p->out = FL(numcycles);
    return OK;
}

/* errormsg
 *
 * errormsg Smsg
 * errormsg Skind, Smsg
 *
 * errormsg "my error message"
 * errormsg "warning", "this is only a warning, event will keep running"
 *
 * Skind can be "error", "warning" or "info"
 * If "error", (the default) a performance error is thrown, which will delete the current event
 * If "warning", a warning will be thrown but the event goes on (a warning is only shown once)
 * If "info", a warning will be thrown each iteration
 *
 * To throw a critical error stopping csound just use exitnow
 *
 * Only k-rate. If "init" rate is needed, just do
 * if timeinstk() == 1 then
 *   errormsg "my init error message"
 * endif
 */

enum Errorkind_t { ERRORMSG_ERROR, ERRORMSG_WARNING, ERRORMSG_INFO, ERRORMSG_INIT, ERRORMSG_UNKNOWN};

typedef struct {
    OPDS h;
    STRINGDAT *S1;
    STRINGDAT *S2;
    enum Errorkind_t kind;
    int warning_done;
    int which;
} ERRORMSG;


static int32_t errormsg_init(CSOUND *csound, ERRORMSG *p) {
    IGN(csound);
    if(!strcmp(p->S1->data, "init")) {
        p->which = ERRORMSG_UNKNOWN;
        return INITERRF("\n   %s\n", p->S2->data);
    }
    if(!strcmp(p->S1->data, "error"))
        p->kind = ERRORMSG_ERROR;
    else if(!strcmp(p->S1->data, "warning"))
        p->kind = ERRORMSG_WARNING;
    else if(!strcmp(p->S1->data, "info"))
        p->kind = ERRORMSG_INFO;
    else
        return INITERR("Unknown type");
    p->warning_done = 0;   // default: mark message as "error"
    p->which = 1;
    return OK;
}

static int32_t errormsg_init0(CSOUND *csound, ERRORMSG *p) {
    IGN(csound);
    p->kind = ERRORMSG_ERROR;
    p->which = 0;
    return OK;
}

static int32_t errormsg_perf(CSOUND *csound, ERRORMSG *p) {
    char *name;
    INSDS *ip;
    char *msg = p->which == 0 ? p->S1->data : p->S2->data;

    switch(p->kind) {
    case ERRORMSG_ERROR:
        return csound->PerfError(csound, &(p->h), "%s\n", msg);
    case ERRORMSG_WARNING:
        if(p->warning_done)
            return OK;
        ip = p->h.insdshead;
        name = (ip->instr->insname != NULL) ? ip->instr->insname : "";
        csound->Warning(csound, "Warning from instr %d (%s), line %d\n    %s\n",
                       ip->insno, name, p->h.optext->t.linenum, msg);
        p->warning_done = 1;
        return OK;
    case ERRORMSG_INFO:
        ip = p->h.insdshead;
        name = (ip->instr->insname != NULL) ? ip->instr->insname : "";
        csound->Warning(csound, "Info from instr %d (%s), line %d\n    %s\n",
                       ip->insno, name, p->h.optext->t.linenum, msg);
        return OK;
    case ERRORMSG_UNKNOWN:
        return NOTOK;
    }
}



// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~




#define S(x) sizeof(x)

static OENTRY localops[] = {
    { "crackle", S(CRACKLE), 0, 3, "a", "P", (SUBR)crackle_init, (SUBR)crackle_perf },

    { "ramptrig.k_kk", S(RAMPTRIGK), 0, 3, "k", "kkP", (SUBR)ramptrig_k_kk_init,
      (SUBR)ramptrig_k_kk },
    { "ramptrig.a_kk", S(RAMPTRIGK), 0, 3, "a", "kkP", (SUBR)ramptrig_a_kk_init,
      (SUBR)ramptrig_a_kk },
    { "ramptrig.sync_kk_kk", S(RAMPTRIGSYNC), 0, 3, "kk", "kkPO",
      (SUBR)ramptrigsync_kk_kk_init, (SUBR)ramptrigsync_kk_kk},

    { "sigmdrive.a_ak",S(SIGMDRIVE), 0, 2, "a", "akO", NULL, (SUBR)sigmdrive_a_ak},
    { "sigmdrive.a_aa",S(SIGMDRIVE), 0, 2, "a", "aaO", NULL, (SUBR)sigmdrive_a_aa},

    { "lfnoise", S(LFNOISE), 0, 3, "a", "kO", (SUBR)lfnoise_init, (SUBR)lfnoise_perf},

    { "schmitt.k", S(SCHMITT), 0, 3, "k", "kkk", (SUBR)schmitt_k_init, (SUBR)schmitt_k_perf},
    { "schmitt.a", S(SCHMITT), 0, 3, "a", "akk", (SUBR)schmitt_k_init, (SUBR)schmitt_a_perf},

    { "standardchaos", S(STANDARDCHAOS), 0, 3, "a", "kkio",
      (SUBR)standardchaos_init, (SUBR)standardchaos_perf},
    { "standardchaos", S(STANDARDCHAOS), 0, 3, "a", "kP",
      (SUBR)standardchaos_init_x, (SUBR)standardchaos_perf},

    { "linenv.k_k", S(RAMPGATE), 0, 3, "k", "kiM", (SUBR)linenv_k_init, (SUBR)linenv_k_k},
    { "linenv.a_k", S(RAMPGATE), 0, 3, "a", "kiM", (SUBR)linenv_a_init, (SUBR)linenv_a_k},

    // aout dioderingmod ain, kfreq, kdiodeon=1, kfeedback=0, knonlinear=0, ioversample=0
    { "diode_ringmod", S(t_diode_ringmod), 0, 3, "a", "akPOOO",
      (SUBR)dioderingmod_init, (SUBR)dioderingmod_perf},

    { "fileexists", S(FILE_EXISTS), 0, 1, "i", "S", (SUBR)file_exists_init},

    { "pread.i", S(PREAD), 0, 1, "i", "iij", (SUBR)pread_i},
    { "pread.k", S(PREAD), 0, 3, "k", "ikJ", (SUBR)pread_init, (SUBR)pread_perf},
    { "pwrite.i", S(PWRITE), 0, 1, "", "im", (SUBR)pwrite_i},
    { "pwrite.k", S(PWRITE), 0, 3, "", "i*",
      (SUBR)pwrite_initcommon, (SUBR)pwrite_perf},

    { "uniqinstance.i", S(UNIQINSTANCE),   0, 1, "i", "io", (SUBR)uniqueinstance_i},
    { "uniqinstance.S_i", S(UNIQINSTANCE), 0, 1, "i", "So", (SUBR)uniqueinstance_S_init},

    { "atstop.s1", S(SCHED_DEINIT), 0, 1, "", "Soj", (SUBR)atstop_s },
    { "atstop.s", S(SCHED_DEINIT),  0, 1, "", "Siim", (SUBR)atstop_s },
    { "atstop.i1", S(SCHED_DEINIT), 0, 1, "", "ioj", (SUBR)atstop_i },
    { "atstop.i", S(SCHED_DEINIT), 0, 1, "", "iiim", (SUBR)atstop_i },

    { "accum", S(ACCUM), 0, 3, "k", "ko", (SUBR)accum_init, (SUBR)accum_perf},
    { "accum", S(ACCUM), 0, 3, "a", "ko", (SUBR)accum_init, (SUBR)accum_perf_audio},

    { "frac2int", S(FUNC12), 0, 1, "i", "ii", (SUBR)frac2int},
    { "frac2int", S(FUNC12), 0, 2, "k", "kk", NULL, (SUBR)frac2int},

    { "memview.i_table", S(TABALIAS),  0, 1, "i[]", "ioo", (SUBR)tabalias_init},
    { "memview.k_table", S(TABALIAS),  0, 1, "k[]", "ioo", (SUBR)tabalias_init},
    { "memview.k", S(ARRAYVIEW), 0, 1, "k[]", ".[]oo", (SUBR)arrayview_init},
    { "memview.i", S(ARRAYVIEW), 0, 1, "i[]", ".[]oo", (SUBR)arrayview_init},

    { "ref.arr", S(REF_NEW_ARRAY), 0, 1, "i", ".[]o", (SUBR)ref_new_array},
    // { "ref.k_send", S(REF1), 0, 3, "i", "k", (SUBR)ref_scalar_init, (SUBR)ref_scalar_perf},
    // { "ref.a_send", S(REF1), 0, 3, "i", "a", (SUBR)ref_audio_init, (SUBR)ref_audio_perf},

    { "deref.arr_i", S(DEREF_ARRAY), 0, 1, "i[]", "io", (SUBR)deref_array},
    { "deref.arr_k", S(DEREF_ARRAY), 0, 1, "k[]", "io", (SUBR)deref_array},
    // { "refread.k_recv", S(REF1), 0, 3, "k", "i", (SUBR)deref_scalar_init, (SUBR)deref_scalar_perf},
    // { "refread.a_recv", S(REF1), 0, 3, "a", "i", (SUBR)deref_audio_init, (SUBR)deref_audio_perf},

    { "refvalid.i", S(REF1), 0, 1, "i", "i", (SUBR)ref_valid_i},
    { "refvalid.k", S(REF1), 0, 3, "k", "k", (SUBR)ref1_init, (SUBR)ref_valid_perf},

    // { "refinc.i", S(REF1), 0, 1, "", "i", (SUBR)ref_inc_i},
    // { "refinc.k", S(REF1), 0, 3, "", "k", (SUBR)ref1_init, (SUBR)ref_inc_perf},

    // { "xtracycles.i", S(OPCk_0), 0, 1, "i", "", (SUBR)xtracycles},

    { "throwerror.s", S(ERRORMSG), 0, 3, "", "S", (SUBR)errormsg_init0, (SUBR)errormsg_perf},
    { "throwerror.ss", S(ERRORMSG), 0, 3, "", "SS", (SUBR)errormsg_init, (SUBR)errormsg_perf},

    { "setslice.k", S(ARRSETSLICE), 0, 2, "", "k[]kOOP", NULL, (SUBR)array_set_slice},
    { "setslice.i", S(ARRSETSLICE), 0, 1, "", "i[]ioop", (SUBR)array_set_slice}

};

LINKAGE
