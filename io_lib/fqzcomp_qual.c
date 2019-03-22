// We use generic maps to turn 0-M into 0-N where N <= M
// before adding these into the context.  These are used
// for positions, running-diffs and quality values.
//
// This can be used as a simple divisor, eg pos/24 to get
// 2 bits of positional data for each quarter along a 100bp
// read, or it can be tailored for specific such as noting
// the first 5 cycles are poor, then we have stability and
// a gradual drop off in the last 20 or so.  Perhaps we then
// map pos 0-4=0, 5-79=1, 80-89=2, 90-99=3.
//
// We don't need to specify how many bits of data we are
// using (2 in the above example), as that is just implicit
// in the values in the map.  Specify not to use a map simply
// disables that context type (our map is essentially 0-M -> 0).

// Example of command line usage:
//
// f=~/scratch/data/q4
// cc -Wall -DTEST_MAIN -O3 -g fqzcomp_qual2.c -I../.. -I.. -I../../build.x86_64 -lm 2>&1
// ./a.out $f > /tmp/_ && ./a.out -d < /tmp/_ > /tmp/__ && cmp /tmp/__ $f

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <ctype.h>
#include <math.h>

#define CTX_BITS 16
#define CTX_SIZE (1<<CTX_BITS)

#ifdef TEST_MAIN
#include <fcntl.h>
/*
 * Standalone hackery.
 * Fixed sizes, just for testing and benchmarking
 */
typedef struct {
    int num_records;
} cram_hdr;

typedef struct {
    int len;
    int qual;
    int flags;
} cram_crec;

#ifndef MAX_REC
#define MAX_REC 1000000
#endif

#ifndef MAX_SEQ
#define MAX_SEQ 100000
#endif

typedef struct {
    cram_hdr *hdr;
    cram_crec crecs[MAX_REC];
} cram_slice;

static cram_hdr fixed_hdr;
static cram_slice fixed_slice = {0};
#define BAM_FREVERSE 0
#define BAM_FREAD2 128

cram_slice *fake_slice(size_t buf_len, int *len, int *r2, int *sel, int nlen) {
    fixed_slice.hdr = &fixed_hdr;
    fixed_hdr.num_records = (nlen == 1) ? (buf_len+len[0]-1) / len[0] : nlen;
    assert(fixed_hdr.num_records <= MAX_REC);
    int i, tlen = 0;
    for (i = 0; i < fixed_hdr.num_records; i++) {
	int idx = i < nlen ? i : nlen-1;
	fixed_slice.crecs[i].len = len[idx];
	fixed_slice.crecs[i].qual = tlen;
	fixed_slice.crecs[i].flags = r2 ? r2[idx]*BAM_FREAD2 : 0;
	fixed_slice.crecs[i].flags |= sel ? (sel[idx]<<16) : 0;
	tlen += len[idx];
    }

    return &fixed_slice;
}

#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#endif

#else
#include "cram_block_compression.h"
#include "fqzcomp_qual.h"
#endif

static const char *name(void) {
    return "fqzcomp-qual";
}

// Global flags
static const int GFLAG_MULTI_PARAM = 1;
static const int GFLAG_HAVE_STAB   = 2;
static const int GFLAG_DO_REV      = 4;

// Param flags
// Add PFLAG_HAVE_DMAP and a dmap[] for delta incr?
static const int PFLAG_DO_DEDUP    = 2;
static const int PFLAG_DO_LEN      = 4;
static const int PFLAG_DO_SEL      = 8;
static const int PFLAG_HAVE_QMAP   = 16;
static const int PFLAG_HAVE_PTAB   = 32;
static const int PFLAG_HAVE_DTAB   = 64;
static const int PFLAG_HAVE_QTAB   = 128;

#define QMAX 256
#define QBITS 12
#define QSIZE (1<<QBITS)

#define NSYM 2
#include "c_simple_model.h"

#undef NSYM
#define NSYM QMAX
//#include "c_escape_model.h"
#include "c_simple_model.h"
//#include "c_cdf_model.h"
//#include "c_cdf16_model.h"

// An array of 0,0,0, 1,1,1,1, 3, 5,5
// is turned into a run-length of 3x0, 4x1, 0x2, 1x4, 0x4, 2x5,
// which then becomes 3 4 0 1 0 2.
//
// NB: size > 255 therefore means we need to repeatedly read to find
// the actual run length.
// Alternatively we could bit-encode instead of byte encode, eg BETA.
static int store_array(unsigned char *out, unsigned int *array, int size) {
    int i, j, k;
    for (i = j = k = 0; i < size; j++) {
	int run_len = i;
	while (i < size && array[i] == j)
	    i++;
	run_len = i-run_len;

	int r;
	do {
	    r = MIN(255, run_len);
	    out[k++] = r;
	    run_len -= r;
	} while (r == 255);
    }
    while (i < size)
	out[k++] = 0, j++;

    // RLE on out.
    //    1 2 3 3 3 3 3 4 4    5
    // => 1 2 3 3 +3... 4 4 +0 5
    int last = -1;
    for (i = j = 0; j < k; i++) {
	out[i] = out[j++];
	if (out[i] == last) {
	    int n = j;
	    while (j < k && out[j] == last)
		j++;
	    out[++i] = j-n;
	} else {
	    last = out[i];
	}
    }
    k = i;

//    fprintf(stderr, "Store_array %d => %d {", size, k);
//    for (i = 0; i < k; i++)
//	fprintf(stderr, "%d,", out[i]);
//    fprintf(stderr, "}\n");
    return k;
}

static int read_array(unsigned char *in, unsigned int *array, int size) {
#if 0
    unsigned char A[1024];
    int i, j, k, last = -1, nb = 0;

    size = MIN(1024, size);

    // Remove level one of run-len encoding
    for (i = j = k = 0; k < size; i++) {
	k += in[i];
	A[j++] = in[i];
	int run = in[i];
	if (in[i] == last) {
	    int r = in[++i];
	    while (r-- && k < size) {
		A[j++] = run;
		k += run;
	    }
	}
	last = run;
    }
    nb = i;

    // Now expand inner level of run-length encoding
    in = A;
    for (i = j = k = 0; j < size; i++) {
	int run_len = 0;
	int run_part;
	do {
	    run_part = in[k++];
	    run_len += run_part;
	} while (run_part == 255);

	while (run_len && j < size)
	    run_len--, array[j++] = i;
    }

    return nb;
#else
    int i, j, k, last = -1, r2 = 0;

    for (i = j = k = 0; j < size; i++) {
	int run_len;
	if (r2) {
	    run_len = last;
	} else {
	    run_len = 0;
	    int r, loop = 0;
	    do {
		r = in[k++];
		//fprintf(stderr, "%d,", r);
		if (++loop == 3)
		    run_len += r*255, r=255;
		else
		    run_len += r;
	    } while (r == 255);
	}
	if (r2 == 0 &&  run_len == last) {
	    r2 = in[k++];
	    //fprintf(stderr, "%d,", r2);
	} else {
	    if (r2) r2--;
	    last = run_len;
	}

	while (run_len && j < size)
	    run_len--, array[j++] = i;
    }
    //fprintf(stderr, "}\n");

    return k;
#endif
}

// FIXME: how to auto-tune these rather than trial and error?
// r2 = READ2
// qa = qual avg (0, 2, 4)
static int strat_opts[][12] = {
//   qb  qs pb ps db ds ql sl pl  dl  r2 qa
    {10, 5, 4,-1, 2, 1, 0, 14, 10, 14, 0,-1}, // basic options (level < 7)
    {8,  5, 7, 0, 0, 0, 0, 14, 8,  14, 1,-1}, // e.g. HiSeq 2000
    {12, 6, 2, 0, 2, 3, 0, 9,  12, 14, 0, 0}, // e.g. MiSeq
    {12, 6, 0, 0, 0, 0, 0, 12, 0,  0,  0, 0}, // e.g. IonTorrent; adaptive O1
    {0,  0, 0, 0, 0, 0, 0, 0,  0,  0,  0, 0}, // custom
};
static int nstrats = sizeof(strat_opts) / sizeof(*strat_opts);

#ifdef __SSE__
#   include <xmmintrin.h>
#else
#   define _mm_prefetch(a,b)
#endif

// A single parameter block
typedef struct {
    // Starting context value
    uint16_t context;

    // flags
    unsigned int pflags;
    unsigned int do_sel, do_dedup, store_qmap, fixed_len;
    unsigned char use_qtab, use_dtab, use_ptab;
    int first_len; // FIXME: move to state

    // context bits and locations
    unsigned int qbits, qloc;
    unsigned int pbits, ploc;
    unsigned int dbits, dloc;
    unsigned int sbits, sloc;

    // models
    int max_sym, nsym, max_sel;

    // tables / maps
    unsigned int qmap[256];
    unsigned int qtab[256];
    unsigned int ptab[1024];
    unsigned int dtab[256];

    // Not stored paramters, but computed as part of encoder
    // parameterisation.
    int qshift;
    int pshift;
    int dshift;
    int sshift;
    unsigned int qmask; // (1<<qbits)-1
    int do_r2, do_qa;
} fqz_param;

// Some global params and a collection of parameter blocks
typedef struct {
    int vers;               // Format version
    unsigned int gflags;    // global params
    int nparam;             // Number of parameter blocks
    int max_sel;            // Number of selector values
    unsigned int stab[256]; // Selector to parameter no. table

    int max_sym;            // max across all sub-params

    fqz_param *p;           // 1 or more parameter blocks
} fqz_gparams;

typedef struct {
    // FIXME: add last_len here
    unsigned int qctx;  // quality sub-context
    unsigned int p;     // pos (bytes remaining)
    unsigned int add_d; // whether to update delta (skip first cycle)
    unsigned int delta; // delta running total
    unsigned int prevq; // previous quality
    unsigned int s;     // selector
    unsigned int qtot, qlen;
} fqz_state;


void dump_table(unsigned int *tab, int size, char *name) {
    int i, last = -99, run = 0;
    fprintf(stderr, "\t%s\t{", name);
    for (i = 0; i < size; i++) {
	if (tab[i] == last) {
	    run++;
	} else if (run == 1 && tab[i] == last+1) {
	    int first = last;
	    do {
		last = tab[i];
		i++;
	    } while (i < size && tab[i] == last+1);
	    i--;

	    // Want 0,1,2,3,3,3 as 0..2 3x3, not 0..3 3x2
	    if (tab[i] == tab[i+1])
		i--;
	    if (tab[i] != first)
		fprintf(stderr, "..%d", tab[i]);
	    run = 1;
	    last = -99;
	} else {
	    if (run > 1)
		fprintf(stderr, " x %d%s%d", run, i?", ":"", tab[i]);
	    else
		fprintf(stderr, "%s%d", i?", ":"", tab[i]);
	    run = 1;
	    last = tab[i];
	}
    }
    if (run > 1)
	fprintf(stderr, " x %d", run);
    fprintf(stderr, "}\n");
}

void dump_map(unsigned int *map, int size, char *name) {
    int i, c = 0;
    fprintf(stderr, "\t%s\t{", name);
    for (i = 0; i < size; i++)
	if (map[i] != INT_MAX)
	    fprintf(stderr, "%s%d=%d", c++?", ":"", i, map[i]);
    fprintf(stderr, "}\n");
}

void dump_params(fqz_gparams *gp) {
    fprintf(stderr, "Global params = {\n");
    fprintf(stderr, "\tvers\t%d\n", gp->vers);
    fprintf(stderr, "\tgflags\t0x%02x\n", gp->gflags);
    fprintf(stderr, "\tnparam\t%d\n", gp->nparam);
    fprintf(stderr, "\tmax_sel\t%d\n", gp->max_sel);
    fprintf(stderr, "\tmax_sym\t%d\n", gp->max_sym);
    if (gp->gflags & GFLAG_HAVE_STAB)
	dump_table(gp->stab, 256, "stab");
    fprintf(stderr, "}\n");

    int i;
    for (i = 0; i < gp->nparam; i++) {
	fqz_param *pm = &gp->p[i];
	fprintf(stderr, "\nParam[%d] = {\n", i);
	fprintf(stderr, "\tcontext\t0x%04x\n", pm->context);
	fprintf(stderr, "\tpflags\t0x%02x\n",  pm->pflags);
	fprintf(stderr, "\tmax_sym\t%d\n",  pm->max_sym);
	fprintf(stderr, "\tqbits\t%d\n",   pm->qbits);
	fprintf(stderr, "\tqshift\t%d\n",  pm->qshift);
	fprintf(stderr, "\tqloc\t%d\n",    pm->qloc);
	fprintf(stderr, "\tsloc\t%d\n",    pm->sloc);
	fprintf(stderr, "\tploc\t%d\n",    pm->ploc);
	fprintf(stderr, "\tdloc\t%d\n",    pm->dloc);

	if (pm->pflags & PFLAG_HAVE_QMAP)
	    dump_map(pm->qmap, 256, "qmap");

	if (pm->pflags & PFLAG_HAVE_QTAB)
	    dump_table(pm->qtab, 256, "qtab");
	if (pm->pflags & PFLAG_HAVE_PTAB)
	    dump_table(pm->ptab, 1024, "ptab");
	if (pm->pflags & PFLAG_HAVE_DTAB)
	    dump_table(pm->dtab, 256, "dtab");
	fprintf(stderr, "}\n");
    }
}

typedef struct {
    SIMPLE_MODEL(QMAX,_) *qual;
    SIMPLE_MODEL(256,_)   len[4];
    SIMPLE_MODEL(2,_)     revcomp;
    SIMPLE_MODEL(256,_)   sel;
    SIMPLE_MODEL(2,_)     dup;
} fqz_model;

int fqz_create_models(fqz_model *m, fqz_gparams *gp) {
    int i;

    if (!(m->qual = malloc(sizeof(*m->qual) * CTX_SIZE)))
	return -1;
    for (i = 0; i < CTX_SIZE; i++)
	SIMPLE_MODEL(QMAX,_init)(&m->qual[i], gp->max_sym+1);

    for (i = 0; i < 4; i++)
	SIMPLE_MODEL(256,_init)(&m->len[i],256);

    SIMPLE_MODEL(2,_init)(&m->revcomp,2);
    SIMPLE_MODEL(2,_init)(&m->dup,2);
    if (gp->max_sel > 0)
	SIMPLE_MODEL(256,_init)(&m->sel, gp->max_sel+1);

    return 0;
}

void fqz_destroy_models(fqz_model *m) {
    free(m->qual);
}

static inline unsigned int fqz_update_ctx(fqz_param *pm, fqz_state *state, int q) {
    unsigned int last = 0; // pm->context
    state->qctx = (state->qctx << pm->qshift) + pm->qtab[q];
    last += (state->qctx & pm->qmask) << pm->qloc;

    // The final shifts have been factored into the tables already.
    last += pm->ptab[MIN(1024, state->p)];      // << pm->ploc
    last += pm->dtab[MIN(255,  state->delta)];  // << pm->dloc
    last += state->s << pm->sloc;

    // On the fly average is slow work.
    // However it can be slightly better than using a selector bit
    // as it's something we can compute on the fly and thus doesn't
    // consume output bits for storing the selector itself.
    //
    // Q4 (novaseq.bam)
    // qtot+=q*q -DQ1=8.84 -DQ2=8.51 -DQ3=7.70; 7203598 (-0.7%)
    // qtot+=q   -DQ1=2.96 -DQ2=2.85 -DQ3=2.69; 7207315
    // vs old delta;                            7255614 (default params)
    // vs 2 bit selector (no delta)             7203006 (-x 0x8261000e80)
    // vs 2 bit selector (no delta)             7199153 (-x 0x7270000e70) -0.8%
    // vs 2 bit selector (no delta)             7219668 (-x 0xa243000ea0)
    //{
    //	double qa = state->qtot / (state->qlen+.01);
    //	//fprintf(stderr, "%f\n", qa);
    //	int x = 0;
    //	if (qa>=Q1) x=3;
    //	else if (qa>=Q2) x=2;
    //	else if (qa>=Q3) x=1;
    //	else x=0;
    //	last += x << pm->dloc; // tmp reuse of delta pos
    //  state->qtot += q*q;
    //  state->qlen++;
    //}

    // Only update delta after 1st base.
    //state->delta += state->add_d * (state->prevq != q);
    //state->add_d = 1;
    state->delta += (state->prevq != q);
    state->prevq = q;

    state->p--;

    return last & (CTX_SIZE-1);
}

// Build quality stats for qhist and set nsym, do_dedup and do_sel params.
static inline
void qual_stats(cram_slice *s,
		unsigned char *in, size_t in_size,
		unsigned int *q_len,
		fqz_param *pm,
		uint32_t qhist[256]) {
#define NP 128
    uint32_t qhistb[NP][256] = {{0}};  // both
    uint32_t qhist1[NP][256] = {{0}};  // READ1 only
    uint32_t qhist2[NP][256] = {{0}};  // READ2 only
    uint64_t t1[NP] = {0};             // Count for READ1
    uint64_t t2[NP] = {0};             // COUNT for READ2
    uint32_t avg[2560] = {0};          // Avg qual *and later* avg-to-selector map.

    int dir = 0;
    int last_len = 0;
    int do_dedup = 0;
    size_t rec;
    size_t i, j;

    // See what info we've been given.
    // Do we have READ1 / READ2?
    // Do we have selector hidden in the top bits of flag?
    int max_sel = 0;
    int has_r2 = 0;
    for (rec = 0; rec < s->hdr->num_records; rec++) {
	if (max_sel < (s->crecs[rec].flags >> 16))
	    max_sel = (s->crecs[rec].flags >> 16);
	if (s->crecs[rec].flags & BAM_FREAD2)
	    has_r2 = 1;
    }
    //fprintf(stderr, "max_sel=%d\n", max_sel);


    // Dedup detection and histogram stats gathering
    int *avg_qual = malloc((s->hdr->num_records+1) * sizeof(int));
    if (!avg_qual)
	return;

    rec = i = j = 0;
    while (i < in_size) {
	if (rec < s->hdr->num_records) {
	    j = q_len[rec];
	    dir = s->crecs[rec].flags & BAM_FREAD2 ? 1 : 0;
	    if (i > 0 && j == last_len
		&& !memcmp(in+i-last_len, in+i, j))
		do_dedup++; // cache which records are dup?
	} else {
	    j = in_size - i;
	    dir = 0;
	}
	last_len = j;

	uint32_t (*qh)[256] = dir ? qhist2 : qhist1;
	uint64_t *th        = dir ? t2     : t1;

	uint32_t tot = 0;
	for (; i < in_size && j > 0; i++, j--) {
	    tot += in[i];
	    qhist[in[i]]++;
	    qhistb[j & (NP-1)][in[i]]++;
	    qh[j & (NP-1)][in[i]]++;
	    th[j & (NP-1)]++;
	}
	tot = last_len ? (tot*10.0)/last_len+.5 : 0;

	avg_qual[rec] = tot;
	avg[MIN(2559, tot)]++;

	rec++;
    }
    pm->do_dedup = ((rec+1)/(do_dedup+1) < 500);

    last_len = 0;

    // Unique symbol count
    for (i = pm->max_sym = pm->nsym = 0; i < 256; i++) {
	if (qhist[i])
	    pm->max_sym = i, pm->nsym++;
    }


    // Auto tune: does average quality helps us?
    if (pm->do_qa != 0) {
	// Histogram of average qual
	int total = 0;
	i = 0;
	// Few symbols means high compression which means
	// selector bits become more significant fraction.
	// Reduce selector bits by skewing the distribution
	// to not be even binning.

	double qf0 = pm->nsym > 4 ? 0.25 : 0.05;
	double qf1 = pm->nsym > 4 ? 0.50 : 0.15;
	double qf2 = pm->nsym > 4 ? 0.75 : 0.60;
	while (i < 2560) {
	    total += avg[i];
	    if (total > qf0 * s->hdr->num_records) {
		//fprintf(stderr, "Q1=%d\n", (int)i);
		break;
	    }
	    //if (i > 320) break;
	    avg[i++] = 0;
	}
	while (i < 2560) {
	    total += avg[i];
	    if (total > qf1 * s->hdr->num_records) {
		//fprintf(stderr, "Q2=%d\n", (int)i);
		break;
	    }
	    //if (i > 360) break;
	    avg[i++] = 3;
	}
	while (i < 2560) {
	    total += avg[i];
	    if (total > qf2 * s->hdr->num_records) {
		//fprintf(stderr, "Q3=%d\n", (int)i);
		break;
	    }
	    if (i > 375) break;
	    avg[i++] = 2;
	}
	while (i < 2560)
	    avg[i++] = 1;

	// Compute simple entropy of merged signal vs split signal.
        i = 0;
	rec = 0;

	int qbin4[4][NP][256] = {{{0}}};
	int qbin2[2][NP][256] = {{{0}}};
	int qbin1   [NP][256] = {{0}};
	int qcnt4[4][NP] = {{0}};
	int qcnt2[4][NP] = {{0}};
	int qcnt1   [NP] = {0};
        while (i < in_size) {
	    if (rec < s->hdr->num_records)
		j = q_len[rec];
	    else
		j = in_size - i;
	    last_len = j;

	    uint32_t tot = avg_qual[rec];
	    int qb4 = avg[MIN(2559, tot)];

	    for (; i < in_size && j > 0; i++, j--) {
		int x = j & (NP-1);
		qbin4[qb4]  [x][in[i]]++;  qcnt4[qb4]  [x]++;
		qbin2[qb4/2][x][in[i]]++;  qcnt2[qb4/2][x]++;
		qbin1       [x][in[i]]++;  qcnt1       [x]++;
	    }
	    rec++;
	}

	double e1 = 0, e2 = 0, e4 = 0;
	for (j = 0; j < NP; j++) {
	    for (i = 0; i < 256; i++) {
		if (qbin1   [j][i]) e1 += qbin1   [j][i] * log(qbin1   [j][i] / (double)qcnt1   [j]);
		if (qbin2[0][j][i]) e2 += qbin2[0][j][i] * log(qbin2[0][j][i] / (double)qcnt2[0][j]);
		if (qbin2[1][j][i]) e2 += qbin2[1][j][i] * log(qbin2[1][j][i] / (double)qcnt2[1][j]);
		if (qbin4[0][j][i]) e4 += qbin4[0][j][i] * log(qbin4[0][j][i] / (double)qcnt4[0][j]);
		if (qbin4[1][j][i]) e4 += qbin4[1][j][i] * log(qbin4[1][j][i] / (double)qcnt4[1][j]);
		if (qbin4[2][j][i]) e4 += qbin4[2][j][i] * log(qbin4[2][j][i] / (double)qcnt4[2][j]);
		if (qbin4[3][j][i]) e4 += qbin4[3][j][i] * log(qbin4[3][j][i] / (double)qcnt4[3][j]);
	    }
	}
	e1 /= -log(2)/8;
	e2 /= -log(2)/8;
	e4 /= -log(2)/8;
	//fprintf(stderr, "E1=%f E2=%f E4=%f\n", e1, e2+s->hdr->num_records/8, e4+s->hdr->num_records/4);

	// Note by using the selector we're robbing bits from elsewhere in
	// the context, which may reduce compression better.
	// We don't know how much by, so this is basically a guess!
	// For now we just say need 5% saving here.
	double qm = pm->do_qa > 0 ? 1 : 0.95;
	if ((pm->do_qa == -1 || pm->do_qa >= 4) &&
	    e4 + s->hdr->num_records/4 < e2*qm + s->hdr->num_records/8 &&
	    e4 + s->hdr->num_records/4 < e1*qm) {
	    for (i = 0; i < s->hdr->num_records; i++) {
		//fprintf(stderr, "%d -> %d -> %d, %d\n", (int)i, avg_qual[i], avg[MIN(2559, avg_qual[i])], s->crecs[i].flags>>16);
		s->crecs[i].flags |= avg[MIN(2559, avg_qual[i])] <<16;
	    }
	    pm->do_sel = 1;
	    max_sel = 3;
	} else if ((pm->do_qa == -1 || pm->do_qa >= 2) && e2 + s->hdr->num_records/8 < e1*qm) {
	    for (i = 0; i < s->hdr->num_records; i++)
		s->crecs[i].flags |= (avg[MIN(2559, avg_qual[i])]>>1) <<16;
	    pm->do_sel = 1;
	    max_sel = 1;
	}

	if (pm->do_qa == -1) {
	    // assume qual, pos, delta in that order.
	    if (pm->pbits > 0 && pm->dbits > 0) {
		// 1 from pos/delta
		pm->sloc = pm->dloc-1;
		pm->pbits--;
		pm->dbits--;
		pm->dloc++;
	    } else if (pm->dbits >= 2) {
		// 2 from delta
		pm->sloc = pm->dloc;
		pm->dbits -= 2;
		pm->dloc += 2;
	    } else if (pm->qbits >= 2) {
		pm->qbits -= 2;
		pm->ploc -= 2;
		pm->sloc = 16-2 - pm->do_r2;
		if (pm->qbits == 6 && pm->qshift == 5)
		    pm->qbits--;
	    }
	    pm->do_qa = 4;
	}
    }

    // Auto tune: does splitting up READ1 and READ2 help us?
    if (has_r2 || pm->do_r2) { // FIXME: && but debug for now
	double e1 = 0, e2 = 0; // entropy sum

	for (j = 0; j < NP; j++) {
	    if (!t1[j] || !t2[j]) continue;
	    for (i = 0; i < 256; i++) {
		if (!qhistb[j][i]) continue;
		e1 -= (qhistb[j][i])*log(qhistb[j][i] / (double)(t1[j]+t2[j]));
		if (qhist1[j][i])
		    e2 -= qhist1[j][i] * log(qhist1[j][i] / (double)t1[j]);
		if (qhist2[j][i])
		    e2 -= qhist2[j][i] * log(qhist2[j][i] / (double)t2[j]);
	    }
	}
	e1 /= log(2)*8; // bytes
	e2 /= log(2)*8;

	//fprintf(stderr, "read1/2 entropy merge %f split %f\n", e1, e2);

	// Note by using the selector we're robbing bits from elsewhere in
	// the context, which may reduce compression better.
	// We don't know how much by, so this is basically a guess!
	// For now we just say need 5% saving here.
	double qm = pm->do_r2 > 0 ? 1 : 0.95;
	if (e2 + (8+s->hdr->num_records/8) < e1*qm) {
	    for (rec = 0; rec < s->hdr->num_records; rec++) {
		int sel = s->crecs[rec].flags >> 16;
		s->crecs[rec].flags =  (s->crecs[rec].flags & 0xffff)
		    | ((s->crecs[rec].flags & BAM_FREAD2)
		       ? ((sel*2)+1) << 16
		       : ((sel*2)+0) << 16);
		if (max_sel < (s->crecs[rec].flags>>16))
		    max_sel = (s->crecs[rec].flags>>16);
	    }
	}
    }

    // We provided explicit selector data or auto-tuned it
    if (max_sel > 0)
	pm->do_sel = 1;

    free(avg_qual);
}

static inline
int fqz_store_parameters1(fqz_param *pm, unsigned char *comp) {
    int comp_idx = 0, i, j;

    // Starting context
    comp[comp_idx++] = pm->context;
    comp[comp_idx++] = pm->context >> 8;

    comp[comp_idx++] = pm->pflags;
    comp[comp_idx++] = pm->max_sym;

    comp[comp_idx++] = (pm->qbits<<4)|pm->qshift;
    comp[comp_idx++] = (pm->qloc<<4)|pm->sloc;
    comp[comp_idx++] = (pm->ploc<<4)|pm->dloc;

    if (pm->store_qmap) {
	for (i = j = 0; i < 256; i++)
	    if (pm->qmap[i] != INT_MAX)
		comp[comp_idx++] = i;
    }

    if (pm->qbits && pm->use_qtab)
	// custom qtab
	comp_idx += store_array(comp+comp_idx, pm->qtab, 256);

    if (pm->pbits && pm->use_ptab)
	// custom ptab
	comp_idx += store_array(comp+comp_idx, pm->ptab, 1024);

    if (pm->dbits && pm->use_dtab)
	// custom dtab
	comp_idx += store_array(comp+comp_idx, pm->dtab, 256);

    return comp_idx;
}

int fqz_store_parameters(fqz_gparams *gp, unsigned char *comp) {
    int comp_idx = 0;
    comp[comp_idx++] = gp->vers; // Format number

    comp[comp_idx++] = gp->gflags;

    if (gp->gflags & GFLAG_MULTI_PARAM)
	comp[comp_idx++] = gp->nparam;

    if (gp->gflags & GFLAG_HAVE_STAB) {
	comp[comp_idx++] = gp->max_sel;
	comp_idx += store_array(comp+comp_idx, gp->stab, 256);
    }

    int i;
    for (i = 0; i < gp->nparam; i++)
	comp_idx += fqz_store_parameters1(&gp->p[i], comp+comp_idx);

    //fprintf(stderr, "Encoded %d bytes of param\n", comp_idx);
    return comp_idx;
}

// Choose a set of parameters based on quality statistics and
// some predefined options (selected via "strat").
static inline
int fqz_pick_parameters(fqz_gparams *gp,
			int vers,
			int strat,
			int level,
			cram_slice *s,
			unsigned char *in,
			size_t in_size,
			unsigned char *comp,
			int *comp_idx_p,
			unsigned int *q_len) {
    //approx sqrt(delta), must be sequential
    int dsqr[] = {
	0, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3,
	4, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5,
	5, 5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
	6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7
    };

    if (strat >= nstrats) strat = nstrats-1;

    // Compute quality length per sequence.
    // This isn't just s->crecs[i].len as some times we emit extra QS
    // records, eg for feature code B.  Instead look at the .qual field.
    size_t i;
    for (i = 0; i < s->hdr->num_records; i++) {
	q_len[i] = (i < s->hdr->num_records-1)
	    ? s->crecs[i+1].qual - s->crecs[i].qual
	    : in_size - s->crecs[i].qual;
    }

    uint32_t qhist[256] = {0};

    // Start with 1 set of parameters.
    // FIXME: add support for multiple params later.
    memset(gp, 0, sizeof(*gp));
    gp->vers = 5; // FQZ format number

    if (!(gp->p = calloc(1, sizeof(fqz_param))))
	return -1;
    gp->nparam = 1;
    gp->max_sel = 0;

    if (vers == 3) // V3.0 doesn't store qual in original orientation
	gp->gflags |= GFLAG_DO_REV;

    fqz_param *pm = gp->p;

    // Programmed strategies, which we then amend based on our
    // statistical analysis of the quality stream.
    pm->qbits  = strat_opts[strat][0];
    pm->qshift = strat_opts[strat][1];
    pm->pbits  = strat_opts[strat][2];
    pm->pshift = strat_opts[strat][3];
    pm->dbits  = strat_opts[strat][4];
    pm->dshift = strat_opts[strat][5];
    pm->qloc   = strat_opts[strat][6];
    pm->sloc   = strat_opts[strat][7];
    pm->ploc   = strat_opts[strat][8];
    pm->dloc   = strat_opts[strat][9];

    // Params for controlling behaviour here.
    pm->do_r2 = strat_opts[strat][10];
    pm->do_qa = strat_opts[strat][11];

    // Quality metrics
    qual_stats(s, in, in_size, q_len, pm, qhist);

    pm->store_qmap = (pm->nsym <= 8 && pm->nsym*2 < pm->max_sym);

    // Check for fixed length.
    pm->first_len = q_len[0];
    for (i = 1; i < s->hdr->num_records; i++) {
	if (q_len[i] != pm->first_len)
	    break;
    }
    pm->fixed_len = (i == s->hdr->num_records);
    pm->first_len = 1; // used as boolean condition now
    pm->use_qtab = 0; // unused by current encoder

    if (strat >= nstrats-1)
	goto manually_set; // used in TEST_MAIN for debugging

    if (pm->pshift < 0)
	pm->pshift = MAX(0, log((double)s->crecs[0].len/(1<<pm->pbits))/log(2)+.5);

    if (pm->nsym <= 4) {
	// NovaSeq
	pm->qshift = 2; // qmax 64, although we can store up to 256 if needed
	if (in_size < 5000000) {
	    pm->pbits =2;
	    pm->pshift=5;
	}
    } else if (pm->nsym <= 8) {
	// HiSeqX
	pm->qbits =MIN(pm->qbits,9);
	pm->qshift=3;
	if (in_size < 5000000)
	    pm->qbits =6;
    }

    if (in_size < 300000) {
	pm->qbits=pm->qshift;
	pm->dbits=2;
    }

 manually_set:
//    fprintf(stderr, "-x 0x%x%x%x%x%x%x%x%x%x%x%x%x\n",
//	    pm->qbits, pm->qshift,
//	    pm->pbits, pm->pshift,
//	    pm->dbits, pm->dshift,
//	    pm->qloc, pm->sloc, pm->ploc, pm->dloc,
//	    pm->do_r2, pm->do_qa);

    for (i = 0; i < sizeof(dsqr)/sizeof(*dsqr); i++)
	if (dsqr[i] > (1<<pm->dbits)-1)
	    dsqr[i] = (1<<pm->dbits)-1;

    if (pm->store_qmap) {
	int j;
	for (i = j = 0; i < 256; i++)
	    if (qhist[i])
		pm->qmap[i] = j++;
	    else
		pm->qmap[i] = INT_MAX;
	pm->max_sym = pm->nsym;
    } else {
	pm->nsym = 255;
	for (i = 0; i < 256; i++)
	    pm->qmap[i] = i;
    }
    if (gp->max_sym < pm->max_sym)
	gp->max_sym = pm->max_sym;

    // Produce ptab from pshift.
    if (pm->qbits) {
	for (i = 0; i < 256; i++) {
	    pm->qtab[i] = i; // 1:1

	    // Alternative mappings:
	    //qtab[i] = i > 30 ? MIN(max_sym,i)-15 : i/2;  // eg for 9827 BAM
	}

    }
    pm->qmask = (1<<pm->qbits)-1;

    if (pm->pbits) {
	for (i = 0; i < 1024; i++)
	    pm->ptab[i] = MIN((1<<pm->pbits)-1, i>>pm->pshift);

	// Alternatively via analysis of quality distributions we
	// may select a bunch of positions that are special and
	// have a non-uniform ptab[].
	// Manual experimentation on a NovaSeq run saved 2.8% here.
    }

    if (pm->dbits) {
	for (i = 0; i < 256; i++)
	    pm->dtab[i] = dsqr[MIN(sizeof(dsqr)/sizeof(*dsqr)-1, i>>pm->dshift)];
    }

    pm->use_ptab = (pm->pbits > 0);
    pm->use_dtab = (pm->dbits > 0);

    pm->pflags =
	(pm->use_qtab   ?PFLAG_HAVE_QTAB :0)|
	(pm->use_dtab   ?PFLAG_HAVE_DTAB :0)|
	(pm->use_ptab   ?PFLAG_HAVE_PTAB :0)|
	(pm->do_sel     ?PFLAG_DO_SEL    :0)|
	(pm->fixed_len  ?PFLAG_DO_LEN    :0)|
	(pm->do_dedup   ?PFLAG_DO_DEDUP  :0)|
	(pm->store_qmap ?PFLAG_HAVE_QMAP :0);

    gp->max_sel = 0;
    if (pm->do_sel) {
	// 2 selectors values, but 1 parameter block.
	// We'll use the sloc instead to encode the selector bits into
	// the context.
	gp->max_sel = 1; // indicator to check recs
	gp->gflags |= GFLAG_HAVE_STAB;
	// NB: stab is already all zero
    }

    if (gp->max_sel) {
	int max = 0;
	for (i = 0; i < s->hdr->num_records; i++) {
	    if (max < (s->crecs[i].flags >> 16))
		max = (s->crecs[i].flags >> 16);
	}
	gp->max_sel = max;
    }

    *comp_idx_p = fqz_store_parameters(gp, comp);

    return 0;
}

unsigned char *compress_block_fqz2f(int vers,
				    int level,
				    cram_slice *s,
				    unsigned char *in,
				    size_t in_size,
				    size_t *out_size) {
    fqz_gparams gp;

    unsigned int last = 0;
    size_t i, j;
    ssize_t rec = 0;

    int last_len = 0;
    int comp_idx = 0;
    RangeCoder rc;

    unsigned char *comp = (unsigned char *)malloc(in_size*1.1+100000);
    if (!comp)
	return NULL;

    unsigned int *q_len = (unsigned int *)malloc(s->hdr->num_records * sizeof(*q_len));
    if (!q_len) {
	free(comp);
	return NULL;
    }

    // Pick and store params
    if (fqz_pick_parameters(&gp, vers & 0xff, vers >> 8, level,
			    s, in, in_size,
			    comp, &comp_idx, q_len) < 0)
	return NULL;

    //dump_params(&gp);

    fqz_param *pm;

    // Optimise tables to remove shifts in loop (NB: cannot do this in next vers)
    for (j = 0; j < gp.nparam; j++) {
	pm = &gp.p[j];

	for (i = 0; i < 1024; i++)
	    pm->ptab[i] <<= pm->ploc;

	for (i = 0; i < 256; i++)
	    pm->dtab[i] <<= pm->dloc;
    }

    // Create models and initialise range coder
    fqz_model model;
    if (fqz_create_models(&model, &gp) < 0)
	return NULL;

    RC_SetOutput(&rc, (char *)comp+comp_idx);
    RC_StartEncode(&rc);

    // For CRAM3.1, reverse upfront if needed
    pm = &gp.p[0];
    if (gp.gflags & GFLAG_DO_REV) {
	i = rec = j = 0;
	while (i < in_size) {
	    int len = rec < s->hdr->num_records-1
		? q_len[rec] : in_size - i;

	    if (s->crecs[rec].flags & BAM_FREVERSE) {
		// Reverse complement sequence - note: modifies buffer
		int I,J;
		unsigned char *cp = in+i;
		for (I = 0, J = len-1; I < J; I++, J--) {
		    unsigned char c;
		    c = cp[I];
		    cp[I] = cp[J];
		    cp[J] = c;
		}
	    }

	    i += len;
	    rec++;
	}
	rec = 0;
    }

    fqz_state state = {0};
    pm = &gp.p[0];
    state.p = 0;
    int x;

    for (i = 0; i < in_size; i++) {
	if (state.p == 0) {
	    if (pm->do_sel) {
		state.s = s->crecs[rec].flags >> 16; // reuse spare bits
		SIMPLE_MODEL(256,_encodeSymbol)(&model.sel, &rc, state.s);
		//fprintf(stderr, "State %d\n", state.s);
	    } else {
		state.s = 0;
	    }
	    x = (gp.gflags & GFLAG_HAVE_STAB) ? gp.stab[state.s] : state.s;
	    pm = &gp.p[x];

	    //fprintf(stderr, "sel %d param %d\n", state.s, x);

	    // Quality buffer maybe longer than sum of reads if we've
	    // inserted a specific base + quality pair.
	    int len = rec < s->hdr->num_records
		? q_len[rec] : in_size - s->crecs[s->hdr->num_records-1].qual;
	    if (!pm->fixed_len || pm->first_len) {
		SIMPLE_MODEL(256,_encodeSymbol)(&model.len[0], &rc, (len>> 0) & 0xff);
		SIMPLE_MODEL(256,_encodeSymbol)(&model.len[1], &rc, (len>> 8) & 0xff);
		SIMPLE_MODEL(256,_encodeSymbol)(&model.len[2], &rc, (len>>16) & 0xff);
		SIMPLE_MODEL(256,_encodeSymbol)(&model.len[3], &rc, (len>>24) & 0xff);
		//fprintf(stderr, "Len %d\n", len);
		pm->first_len = 0;
	    }

	    if (gp.gflags & GFLAG_DO_REV) {
		// no need to reverse complement for V4.0 as the core format
		// already has this feature.
		if (s->crecs[rec].flags & BAM_FREVERSE)
		    SIMPLE_MODEL(2,_encodeSymbol)(&model.revcomp, &rc, 1);
		else
		    SIMPLE_MODEL(2,_encodeSymbol)(&model.revcomp, &rc, 0);
		//fprintf(stderr, "Rev %d\n", (s->crecs[rec].flags & BAM_FREVERSE) ? 1 : 0);
	    }

	    rec++;

	    state.qtot = 0;
	    state.qlen = 0;

	    state.p = len;
	    state.add_d = 0;
	    state.delta = 0;
	    state.qctx = 0;
	    state.prevq = 0;

	    last = pm->context;

	    if (pm->do_dedup) {
		// Possible dup of previous read?
		if (i && len == last_len && !memcmp(in+i-last_len, in+i, len)) {
		    SIMPLE_MODEL(2,_encodeSymbol)(&model.dup, &rc, 1);
		    i += len-1;
		    state.p = 0;
		    //fprintf(stderr, "Dup 1\n");
		    continue;
		} else {
		    SIMPLE_MODEL(2,_encodeSymbol)(&model.dup, &rc, 0);
		    //fprintf(stderr, "Dup 0\n");
		}

		last_len = len;
	    }
	}

	unsigned char q = in[i];
	unsigned char qm = pm->qmap[q];

	SIMPLE_MODEL(QMAX,_encodeSymbol)(&model.qual[last], &rc, qm);
	//fprintf(stderr, "Sym %d with ctx %04x delta %d prevq %d q %d\n", qm, last, state.delta, state.prevq, qm);
	//fprintf(stderr, "pos=%d, delta=%d\n", state.p, state.delta);
	last = fqz_update_ctx(pm, &state, qm);
    }

    RC_FinishEncode(&rc);

    // For CRAM3.1, undo our earlier reversal step
    if (gp.gflags & GFLAG_DO_REV) {
	i = rec = j = 0;
	while (i < in_size) {
	    int len = rec < s->hdr->num_records-1
		? s->crecs[rec].len
		: in_size - i;

	    if (s->crecs[rec].flags & BAM_FREVERSE) {
		// Reverse complement sequence - note: modifies buffer
		int I,J;
		unsigned char *cp = in+i;
		for (I = 0, J = len-1; I < J; I++, J--) {
		    unsigned char c;
		    c = cp[I];
		    cp[I] = cp[J];
		    cp[J] = c;
		}
	    }

	    i += len;
	    rec++;
	}
    }

    // Clear selector abuse of flags
    for (rec = 0; rec < s->hdr->num_records; rec++)
	s->crecs[rec].flags &= 0xffff;

    *out_size = comp_idx + RC_OutSize(&rc);
    //fprintf(stderr, "%d -> %d\n", (int)in_size, (int)*out_size);

    fqz_destroy_models(&model);
    free(q_len);

    return comp;
}

// Read fqz paramaters.
//
// FIXME: pass in and check in_size.
//
// Returns number of bytes read on success,
//         -1 on failure.
static inline
int fqz_read_parameters1(fqz_param *pm, unsigned char *in) {
    int in_idx = 0;
    size_t i;

    // Starting context
    pm->context = in[in_idx] + (in[in_idx+1]<<8);
    in_idx += 2;

    // Bit flags
    pm->pflags     = in[in_idx++];
    pm->use_qtab   = pm->pflags & PFLAG_HAVE_QTAB;
    pm->use_dtab   = pm->pflags & PFLAG_HAVE_DTAB;
    pm->use_ptab   = pm->pflags & PFLAG_HAVE_PTAB;
    pm->do_sel     = pm->pflags & PFLAG_DO_SEL;
    pm->fixed_len  = pm->pflags & PFLAG_DO_LEN;
    pm->do_dedup   = pm->pflags & PFLAG_DO_DEDUP;
    pm->store_qmap = pm->pflags & PFLAG_HAVE_QMAP;
    pm->max_sym    = in[in_idx++];

    // Sub-context sizes and locations
    pm->qbits      = in[in_idx]>>4;
    pm->qmask      = (1<<pm->qbits)-1;
    pm->qshift     = in[in_idx++]&15;
    pm->qloc       = in[in_idx]>>4;
    pm->sloc       = in[in_idx++]&15;
    pm->ploc       = in[in_idx]>>4;
    pm->dloc       = in[in_idx++]&15;

    // Maps and tables
    if (pm->store_qmap) {
	for (i = 0; i < 256; i++) pm->qmap[i] = INT_MAX; // so dump_map works
	for (i = 0; i < pm->max_sym; i++)
	    pm->qmap[i] = in[in_idx++];
    } else {
	for (i = 0; i < 256; i++)
	    pm->qmap[i] = i;
    }

    if (pm->qbits) {
	if (pm->use_qtab)
	    in_idx += read_array(in+in_idx, pm->qtab, 256);
	else
	    for (i = 0; i < 256; i++)
		pm->qtab[i] = i;
    }

    if (pm->use_ptab)
	in_idx += read_array(in+in_idx, pm->ptab, 1024);
    else
	for (i = 0; i < 1024; i++)
	    pm->ptab[i] = 0;

    if (pm->use_dtab)
        in_idx += read_array(in+in_idx, pm->dtab, 256);
    else
	for (i = 0; i < 256; i++)
	    pm->dtab[i] = 0;

    pm->first_len = 1;

    return in_idx;
}

int fqz_read_parameters(fqz_gparams *gp, unsigned char *in) {
    int in_idx = 0;
    int i;

    // Format version
    gp->vers = in[in_idx++];
    if (gp->vers != 5) {
	fprintf(stderr, "This version of fqzcomp only supports format 5\n");
	return -1;
    }

    // Global glags
    gp->gflags = in[in_idx++];

    // Number of param blocks and param selector details
    gp->nparam = (gp->gflags & GFLAG_MULTI_PARAM) ? in[in_idx++] : 1;
    gp->max_sel = gp->nparam > 1 ? gp->nparam : 0;

    if (gp->gflags & GFLAG_HAVE_STAB) {
	gp->max_sel = in[in_idx++];
	in_idx += read_array(in+in_idx, gp->stab, 256);
    } else {
	for (i = 0; i < gp->nparam; i++)
	    gp->stab[i] = i;
	for (; i < 256; i++)
	    gp->stab[i] = gp->nparam-1;
    }

    // Load the individual parameter locks
    if (!(gp->p = malloc(gp->nparam * sizeof(*gp->p))))
	return -1;

    gp->max_sym = 0;
    for (i = 0; i < gp->nparam; i++) {
	int e = fqz_read_parameters1(&gp->p[i], in + in_idx);
	if (e < 0)
	    return -1;
	in_idx += e;

	if (gp->max_sym < gp->p[i].max_sym)
	    gp->max_sym = gp->p[i].max_sym;
    }

    //fprintf(stderr, "Decoded %d bytes of param\n", in_idx);
    return in_idx;
}

unsigned char *uncompress_block_fqz2f(cram_slice *s,
				      unsigned char *in,
				      size_t in_size,
				      size_t *out_size) {
    fqz_gparams gp;
    fqz_param *pm;
    memset(&gp, 0, sizeof(gp));

    unsigned char *uncomp = NULL;
    RangeCoder rc;
    size_t i, rec = 0, len = *out_size, in_idx = 0;
    unsigned int last = 0;

    // Decode parameter blocks
    if ((in_idx = fqz_read_parameters(&gp, in)) < 0)
	return NULL;
    //dump_params(&gp);

    // Optimisations to remove shifts from main loop
    for (i = 0; i < gp.nparam; i++) {
	int j;
	pm = &gp.p[i];
	for (j = 0; j < 1024; j++)
	    pm->ptab[j] <<= pm->ploc;
	for (j = 0; j < 256; j++)
	    pm->dtab[j] <<= pm->dloc;
    }

    // Initialise models and entropy coder
    fqz_model model;
    if (fqz_create_models(&model, &gp) < 0)
	return NULL;

    RC_SetInput(&rc, (char *)in+in_idx);
    RC_StartDecode(&rc);


    // Allocate buffers
    uncomp = (unsigned char *)malloc(*out_size);
    if (!uncomp)
	return NULL;

    int nrec = 1000;
    char *rev_a = malloc(nrec);
    int *len_a = malloc(nrec * sizeof(int));
    if (!rev_a || !len_a)
	return NULL;

    // Main decode loop
    fqz_state state;
    state.delta = 0;
    state.prevq = 0;
    state.qctx = 0;
    state.p = 0;
    state.s = 0;

    int rev = 0;
    int last_len = 0;
    int x = 0;
    pm = &gp.p[x];
    for (rec = i = 0; i < len; i++) {
	if (rec >= nrec) {
	    nrec *= 2;
	    rev_a = realloc(rev_a, nrec);
	    len_a = realloc(len_a, nrec*sizeof(int));
	    if (!rev_a || !len_a)
		return NULL;
	}

	if (state.p == 0) {
	    // New record
	    if (pm->do_sel) {
		state.s = SIMPLE_MODEL(256,_decodeSymbol)(&model.sel, &rc);
		//fprintf(stderr, "State %d\n", state.s);
	    } else {
		state.s = 0;
	    }
	    x = (gp.gflags & GFLAG_HAVE_STAB) ? gp.stab[MIN(255, state.s)] : state.s;
	    if (x >= gp.nparam)
		return NULL;
	    pm = &gp.p[x];

	    int len = last_len;
	    if (!pm->fixed_len || pm->first_len) {
		len  = SIMPLE_MODEL(256,_decodeSymbol)(&model.len[0], &rc);
		len |= SIMPLE_MODEL(256,_decodeSymbol)(&model.len[1], &rc)<<8;
		len |= SIMPLE_MODEL(256,_decodeSymbol)(&model.len[2], &rc)<<16;
		len |= SIMPLE_MODEL(256,_decodeSymbol)(&model.len[3], &rc)<<24;
		//fprintf(stderr, "Len %d\n", len);
		pm->first_len = 0;
		last_len = len;
	    }
#ifdef TEST_MAIN
		s->crecs[rec].len = len;
#endif

	    if (gp.gflags & GFLAG_DO_REV) {
		rev = SIMPLE_MODEL(2,_decodeSymbol)(&model.revcomp, &rc);
		//fprintf(stderr, "rev %d\n", rev);
		rev_a[rec] = rev;
		len_a[rec] = len;
	    }

	    if (pm->do_dedup) {
		if (SIMPLE_MODEL(2,_decodeSymbol)(&model.dup, &rc)) {
		    //fprintf(stderr, "Dup 1\n");
		    // Dup of last line
		    memcpy(uncomp+i, uncomp+i-len, len);
		    i += len-1;
		    state.p = 0;
		    rec++;
		    continue;
		} else {
		    //fprintf(stderr, "Dup 0\n");
		}
	    }

	    rec++;

	    state.p = len;
	    state.add_d = 0;
	    state.delta = 0;
	    state.prevq = 0;
	    state.qctx = 0;

	    last = pm->context;
	}

	// Decode and output quality
	unsigned char Q = SIMPLE_MODEL(QMAX,_decodeSymbol)(&model.qual[last], &rc);
	unsigned char q = pm->qmap[Q];
	//fprintf(stderr, "Sym %d with ctx %04x delta %d prevq %d q %d\n", Q, last, state.delta, state.prevq, Q);
        uncomp[i] = q;

	// Compute new quality context
	last = fqz_update_ctx(pm, &state, Q);
    }
    rev_a[rec] = rev;
    len_a[rec] = len;

    if (gp.gflags & GFLAG_DO_REV) {
	for (i = rec = 0; i < len; i += len_a[rec++]) {
	    if (!rev_a[rec])
		continue;

	    int I, J;
	    unsigned char *cp = uncomp+i;
	    for (I = 0, J = len_a[rec]-1; I < J; I++, J--) {
		unsigned char c;
		c = cp[I];
		cp[I] = cp[J];
		cp[J] = c;
	    }
	}
    }

    RC_FinishDecode(&rc);
    fqz_destroy_models(&model);
    //free(model.qual);
    free(rev_a);
    free(len_a);

#ifdef TEST_MAIN
    s->hdr->num_records = rec;
#endif

    return uncomp;
}


#ifndef TEST_MAIN
static cram_compressor c = {
    'q', //FOUR_CC("FQZq"),
    1<<DS_QS, // quality only
    1.0,
    name,
    //compress_block_fqz2_BIN,

    //compress_block_fqz2,
    //uncompress_block_fqz2,

    compress_block_fqz2f,
    uncompress_block_fqz2f,

    //compress_block,
    //uncompress_block,
};

cram_compressor *cram_compressor_init(void) {
    return &c;
}

char *fqz_compress(int vers, cram_slice *s, char *in, size_t uncomp_size,
		   size_t *comp_size, int level) {
    return (char *)compress_block_fqz2f(vers, level, s, (unsigned char *)in,
					uncomp_size, comp_size);
}

char *fqz_decompress(char *in, size_t comp_size, size_t *uncomp_size) {
    return (char *)uncompress_block_fqz2f(NULL, (unsigned char *)in,
					  comp_size, uncomp_size);
}

#else // TEST_MAIN
#include <unistd.h>

#define BS 1024*1024
static unsigned char *load(char *fn, size_t *lenp) {
    unsigned char *data = NULL;
    uint64_t dsize = 0;
    uint64_t dcurr = 0;
    signed int len;

    //build_rcp_freq();

    int fd = open(fn, O_RDONLY);
    if (!fd) {
	perror(fn);
	return NULL;
    }

    do {
	if (dsize - dcurr < BS) {
	    dsize = dsize ? dsize * 2 : BS;
	    data = realloc(data, dsize);
	}

	len = read(fd, data + dcurr, BS);
	if (len > 0)
	    dcurr += len;
    } while (len > 0);

    if (len == -1) {
	perror("read");
    }
    close(fd);

    *lenp = dcurr;
    return data;
}

#define BLK_SIZE 200*1000000
//#define BLK_SIZE 100*100000

int count_lines(unsigned char *in, size_t len) {
    size_t i;
    int lines = 0;

    for (i = 0; i < len; i++)
	if (in[i] == '\n')
	    lines++;

    return lines;
}

// QUAL [is_read2 [selector]]
void parse_lines(unsigned char *in, size_t len,
		 int *rec_len, int *rec_r2, int *rec_sel, size_t *new_len) {
    size_t i, j, start;
    int rec = 0;

    for (start = i = j = 0; i < len; i++) {
	if (in[i] == '\n' || in[i] == ' ' || in[i] == '\t') {
	    rec_len[rec] = i-start;

	    // Read2 marker
	    while (i < len && in[i] != '\n' && isspace(in[i]))
		i++;

	    if (in[i] != '\n')
		rec_r2[rec] = atoi((char *)&in[i]);
	    else
		rec_r2[rec] = 0;

	    while (i < len && !isspace(in[i]))
		i++;

	    // selector
	    while (i < len && in[i] != '\n' && isspace(in[i]))
		i++;

	    if (in[i] != '\n')
		rec_sel[rec] = atoi((char *)&in[i]);
	    else
		rec_sel[rec] = 0;

	    while (i < len && in[i] != '\n')
		i++;

	    start = i+1;
	    rec++;
	} else {
	    in[j++] = in[i]-33; // ASCII phred to qual
	}
    }
    *new_len = j;
}

int main(int argc, char **argv) {
    unsigned char *in, *out;
    size_t in_len, out_len;
    int decomp = 0, vers = 4;

    while (argc > 1 && argv[1][0] == '-') {
	if (argc > 1 && strcmp(argv[1], "-d") == 0) {
	    decomp = 1;
	    argv++;
	    argc--;
	}
	if (argc > 2 && strcmp(argv[1], "-s") == 0) {
	    vers += atoi(argv[2])*256;
	    argv+=2;
	    argc-=2;
	}
	if (argc > 2 && strcmp(argv[1], "-x") == 0) {
	    // Examples: -x 0x5570000d5014 q40+dir = 30686053
	    //           -x 0x5270000d5014 q4+dir  =  7165913
	    uint64_t x = strtol(argv[2], NULL, 0);
	    int olen = strlen(argv[2])-2; // -2 for "0x"
	    argv+=2;
	    argc-=2;
	    int z;
	    for (z=0; z<olen; z++) {
		strat_opts[nstrats-1][(olen-1)-z] = x&15;
		x>>=4;
	    }
	    vers = (nstrats-1)*256 + 4;
	}
    }
    in = load(argc > 1 ? argv[1] : "/dev/stdin", &in_len);
    if (!in)
	exit(1);

    int blk_size = BLK_SIZE; // MAX
    if (argc > 3)
	blk_size = atoi(argv[3]);
    if (blk_size > BLK_SIZE)
	blk_size = BLK_SIZE;

    if (decomp) {
	unsigned char *in2 = in;
	while (in_len > 0) {
	    // Read sizes as 32-bit
	    size_t out_len = *(uint32_t *)in2;  in2 += 4;
	    size_t in2_len = *(uint32_t *)in2;  in2 += 4;

	    fprintf(stderr, "out_len %ld, in_len %ld\n", out_len, in2_len);

	    int fake_len = 99999; // we don't care; it'll be corrected
	    cram_slice *s = fake_slice(out_len, &fake_len, NULL, NULL, 1);
	    out = uncompress_block_fqz2f(s, in2, in_len-8, &out_len);

	    // Convert from binary back to ASCII with newlines
	    int i, j;
	    for (i = j = 0; i < s->hdr->num_records; i++) {
		int k;
		char seq[MAX_SEQ];
		for (k = 0; k < s->crecs[i].len; k++)
		    seq[k] = out[j+k]+33;
		seq[k] = 0;
		puts(seq);
		j += s->crecs[i].len;
	    }
	    free(out);
	    in2 += in2_len;
	    in_len -= in2_len+8;

	    break; // One cycle only until we fix blocking to be \n based
	}
    } else {
	// Convert from ASCII newline separated file to binary block.
	// We return an array of line lengths and optionally param selectors.
	int nlines = count_lines(in, in_len);
	fprintf(stderr, "nlines=%d\n", nlines);
	int *rec_len = calloc(nlines, sizeof(*rec_len));
	int *rec_r2  = calloc(nlines, sizeof(*rec_r2));
	int *rec_sel = calloc(nlines, sizeof(*rec_sel));
	parse_lines(in, in_len, rec_len, rec_r2, rec_sel, &in_len);

	unsigned char *in2 = in;
	long t_out = 0;
	out = NULL;
	while (in_len > 0) {
	    // FIXME: blk_size no longer working in test.  One cycle only!
	    size_t in2_len = in_len <= blk_size ? in_len : blk_size;
	    cram_slice *s = fake_slice(in2_len, rec_len, rec_r2, rec_sel, nlines);
	    out = compress_block_fqz2f(vers, 0, s, in2, in2_len, &out_len);

	    // Write out 32-bit sizes.
	    uint32_t u32;
	    u32 = in2_len; if (write(1, &u32, 4) != 4) return 1;
	    u32 = out_len; if (write(1, &u32, 4) != 4) return 1;
	    //if (write(1, &in2_len, 8)  < 0) return 1;
	    //if (write(1, &out_len, 8)  < 0) return 1;
	    if (write(1, out, out_len) < 0) return 1;
	    in_len -= in2_len;
	    in2 += in2_len;
	    t_out += out_len+16;

	    break; // One cycle only until we fix blocking to be \n based
	}
	free(out);
	fprintf(stderr, "Total output = %ld\n", t_out);
    }

    free(in);

    return 0;
}
#endif
