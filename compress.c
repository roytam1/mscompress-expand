/*
 * compress.c - functional C89 reconstruction of Microsoft's
 * COMPRESS.EXE (version 1.11 command line) built by inverting the
 * verified decoders.
 *
 * Algorithms:
 *   -a1  SZDD "A" format        (LZSS, window start 4096-16)
 *   -a2  KWAJ method 2          (LZSS, window start 4096-18)
 *   -a3  KWAJ method 3          (LZ + canonical Huffman) - default
 *   -a4  KWAJ method 4          (MS-ZIP, fixed-Huffman raw DEFLATE)
 *
 * The first three match COMPRESS.EXE's algorithms (1 = old SZDD,
 * 2 = "Steven Zeck" LZSS, 3 = "Jeff Johnson" LZSS+Huffman, the default);
 * -a4 is an extension compatible with DECOMP.EXE and kwaj4.py.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <io.h>
#include <sys/stat.h>
#include "lzencode.h"

#define C_WINDOW_4096 4096
#define C_START_EXPAND (C_WINDOW_4096 - 16)
#define C_START_KWAJ   (C_WINDOW_4096 - 18)
#define C_MSZIP_BLOCK  32768

static const unsigned char C_SIG_SZDD[8] =
    { 0x53, 0x5A, 0x44, 0x44, 0x88, 0xF0, 0x27, 0x33 };
static const unsigned char C_SIG_KWAJ[8] =
    { 0x4B, 0x57, 0x41, 0x4A, 0x88, 0xF0, 0x27, 0xD1 };

/* ------------------------------------------------------------------ */
/* bit writer                                                          */
typedef struct
{
    unsigned char *buf;
    unsigned long len, cap;
    unsigned int acc;
    int nbits;
    int msb;                   /* 1 = pack bits into byte from bit 7 down */
} BitWriter;

static int BwInit(BitWriter *bw)
{
    bw->cap = 4096;
    bw->len = 0;
    bw->acc = 0;
    bw->nbits = 0;
    bw->msb = 0;
    bw->buf = (unsigned char *)malloc((size_t)bw->cap);
    return bw->buf != NULL;
}

static int BwByte(BitWriter *bw, unsigned char c)
{
    if (bw->len >= bw->cap)
    {
        unsigned char *tmp;
        bw->cap *= 2;
        tmp = (unsigned char *)realloc(bw->buf, (size_t)bw->cap);
        if (!tmp) return -1;
        bw->buf = tmp;
    }
    bw->buf[bw->len++] = c;
    return 0;
}

static int BwBit(BitWriter *bw, int bit)
{
    bw->acc |= (unsigned int)(bit & 1) << (bw->msb ? (7 - bw->nbits)
                                                    : bw->nbits);
    bw->nbits++;
    if (bw->nbits == 8)
    {
        if (BwByte(bw, (unsigned char)bw->acc)) return -1;
        bw->acc = 0;
        bw->nbits = 0;
    }
    return 0;
}

/* append n low bits of v, least significant bit first */
static int BwLsb(BitWriter *bw, unsigned int v, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (BwBit(bw, (int)((v >> i) & 1u))) return -1;
    return 0;
}

/* append an n-bit Huffman code, most significant bit first */
static int BwCode(BitWriter *bw, unsigned int code, int n)
{
    int i;
    for (i = n - 1; i >= 0; i--)
        if (BwBit(bw, (int)((code >> i) & 1u))) return -1;
    return 0;
}

static int BwFlush(BitWriter *bw)
{
    if (bw->nbits)
        if (BwByte(bw, (unsigned char)bw->acc)) return -1;
    bw->acc = 0;
    bw->nbits = 0;
    return 0;
}

static void BwFree(BitWriter *bw)
{
    free(bw->buf);
    bw->buf = NULL;
}

/* ------------------------------------------------------------------ */
/* LZSS writer (SZDD and KWAJ method 2)                               */
typedef struct
{
    int match;
    unsigned char a, b;
} LzItem;

static int WriteLzss(LzToken *tok, unsigned long n, FILE *out)
{
    LzItem items[8];
    int ctrl = 0, nbits = 0;
    unsigned long i;

    for (i = 0; i < n; i++)
    {
        LzItem *it = &items[nbits];
        if (tok[i].match)
        {
            unsigned int len = tok[i].len;
            unsigned int p = tok[i].pos & 0xFFFu;
            if (len < 3 || len > 18)
                return -1;
            it->match = 1;
            it->a = (unsigned char)(p & 0xFF);
            it->b = (unsigned char)(((p >> 8) & 0x0F) << 4 | (len - 3));
        }
        else
        {
            ctrl |= 1 << nbits;
            it->match = 0;
            it->a = tok[i].lit;
            it->b = 0;
        }
        if (++nbits == 8)
        {
            int j;
            if (fputc(ctrl, out) == EOF) return -1;
            for (j = 0; j < 8; j++)
            {
                if (fputc(items[j].a, out) == EOF) return -1;
                if (items[j].match && fputc(items[j].b, out) == EOF) return -1;
            }
            ctrl = 0;
            nbits = 0;
        }
    }
    if (nbits)
    {
        int j;
        if (fputc(ctrl, out) == EOF) return -1;
        for (j = 0; j < nbits; j++)
        {
            if (fputc(items[j].a, out) == EOF) return -1;
            if (items[j].match && fputc(items[j].b, out) == EOF) return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* KWAJ method 3 writer.  The five Huffman trees are derived from the
 * actual symbol frequencies (Shannon-Fano, which keeps every code at or
 * below 8 bits) and their lengths are written with type 2 run encoding.
 * This replaces the earlier uniform-length trees and saves about 1 KB. */
#define C_KWAJ_TREES 5
static const int C_TreeSize[C_KWAJ_TREES] = { 16, 16, 32, 64, 256 };
static const int C_TreeLit = 2, C_TreeOff = 3, C_TreeSymbol = 4;

/* Shannon-Fano split: order[] holds symbols sorted by descending
 * frequency.  It yields a valid prefix code with depth <= log2(n). */
static void SfSplit(int *order, unsigned long *freq,
                    int lo, int hi, int depth, unsigned char *lengths)
{
    int i, split;
    unsigned long total = 0, acc = 0, half;

    if (lo > hi) return;
    if (lo == hi)
    {
        lengths[order[lo]] = (unsigned char)(depth ? depth : 1);
        return;
    }
    for (i = lo; i <= hi; i++)
        total += freq[order[i]];
    half = total / 2;
    split = lo;
    for (i = lo; i < hi; i++)
    {
        acc += freq[order[i]];
        if (acc >= half) { split = i; break; }
    }
    if (split >= hi) split = hi - 1;
    SfSplit(order, freq, lo, split, depth + 1, lengths);
    SfSplit(order, freq, split + 1, hi, depth + 1, lengths);
}

static void BuildCodes(unsigned char *lengths, int n, unsigned int *codes)
{
    int count[17] = {0}, next[17], i, l, code = 0;

    for (i = 0; i < n; i++)
        count[lengths[i]]++;
    for (l = 1; l <= 16; l++)
    {
        next[l] = code;
        code = (code + count[l]) << 1;
    }
    for (i = 0; i < n; i++)
        if (lengths[i])
            codes[i] = (unsigned int)next[lengths[i]]++;
}

static void BuildTree(unsigned long *freq, int n,
                      unsigned char *lengths, unsigned int *codes)
{
    int order[288], i, j;

    for (i = 0; i < n; i++) { order[i] = i; lengths[i] = 0; }
    /* insertion sort by descending frequency */
    for (i = 1; i < n; i++)
    {
        int s = order[i];
        unsigned long f = freq[s];
        j = i - 1;
        while (j >= 0 && freq[order[j]] < f)
        {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = s;
    }
    i = 0;
    while (i < n && freq[order[i]]) i++;
    if (i > 0)
        SfSplit(order, freq, 0, i - 1, 0, lengths);
    BuildCodes(lengths, n, codes);
}

/* type 2: 4-bit first length, then 2 bits (-1/0/+1 or escape + 4 bits) */
static int WriteLengths2(BitWriter *bw, unsigned char *lengths, int n)
{
    int i, prev, delta;
    if (BwCode(bw, lengths[0], 4)) return -1;
    prev = (int)lengths[0];
    for (i = 1; i < n; i++)
    {
        delta = (int)lengths[i] - prev;
        if (delta >= -1 && delta <= 1)
        {
            if (BwCode(bw, (unsigned int)(delta + 1), 2)) return -1;
        }
        else
        {
            if (BwCode(bw, 3u, 2)) return -1;
            if (BwCode(bw, lengths[i], 4)) return -1;
        }
        prev = (int)lengths[i];
    }
    return 0;
}

static int WriteHuffman(LzToken *tok, unsigned long n, FILE *out)
{
    unsigned long freq[C_KWAJ_TREES][256];
    unsigned char lengths[C_KWAJ_TREES][256];
    unsigned int codes[C_KWAJ_TREES][256];
    BitWriter bw;
    unsigned long i, k;
    int t, rc = -1, selected;

    memset(freq, 0, sizeof(freq));

    /* first pass: gather symbol frequencies exactly as the decoder sees
     * them, including its MATCHLEN / MATCHLEN2 selection rule */
    selected = 0;
    i = 0;
    while (i < n)
    {
        if (!tok[i].match)
        {
            unsigned long cnt = 0;
            int x;
            while (i < n && !tok[i].match && cnt < 32) { i++; cnt++; }
            x = (int)(cnt - 1);
            freq[selected][0]++;          /* literal-run selector */
            freq[C_TreeLit][x]++;
            for (k = i - cnt; k < i; k++)
                freq[C_TreeSymbol][tok[k].lit]++;
            selected = (x != 31) ? 1 : 0;
        }
        else
        {
            unsigned int len = tok[i].len, dist = tok[i].dist;
            if (len > 17 || dist > 4095u) return -1;
            freq[selected][len - 2]++;
            freq[C_TreeOff][dist >> 6]++;
            selected = 0;
            i++;
        }
    }

    for (t = 0; t < C_KWAJ_TREES; t++)
        BuildTree(freq[t], C_TreeSize[t], lengths[t], codes[t]);

    if (!BwInit(&bw)) return -1;
    bw.msb = 1;                 /* KWAJ Huffman bits are read MSB first */
    /* six nibbles: five length lists of type 2, sixth is padding */
    if (BwCode(&bw, 0x22u, 8) || BwCode(&bw, 0x22u, 8) ||
        BwCode(&bw, 0x20u, 8)) goto done;
    for (t = 0; t < C_KWAJ_TREES; t++)
        if (WriteLengths2(&bw, lengths[t], C_TreeSize[t])) goto done;

    selected = 0;
    i = 0;
    while (i < n)
    {
        if (!tok[i].match)
        {
            unsigned long start = i, cnt = 0;
            int x;
            while (i < n && !tok[i].match && cnt < 32) { i++; cnt++; }
            x = (int)(cnt - 1);
            if (BwCode(&bw, codes[selected][0], lengths[selected][0]))
                goto done;                      /* literal-run selector */
            if (BwCode(&bw, codes[C_TreeLit][x], lengths[C_TreeLit][x]))
                goto done;
            for (k = 0; k < cnt; k++)
                if (BwCode(&bw, codes[C_TreeSymbol][tok[start + k].lit],
                           lengths[C_TreeSymbol][tok[start + k].lit]))
                    goto done;
            selected = (x != 31) ? 1 : 0;
        }
        else
        {
            unsigned int len = tok[i].len, dist = tok[i].dist;
            if (BwCode(&bw, codes[selected][len - 2],
                       lengths[selected][len - 2])) goto done;
            if (BwCode(&bw, codes[C_TreeOff][dist >> 6],
                       lengths[C_TreeOff][dist >> 6])) goto done;
            /* the low six distance bits are plain bits, not a tree symbol */
            if (BwCode(&bw, dist & 0x3Fu, 6)) goto done;
            selected = 0;
            i++;
        }
    }

    if (BwFlush(&bw)) goto done;
    if (fwrite(bw.buf, 1, bw.len, out) != bw.len) goto done;
    rc = 0;
done:
    BwFree(&bw);
    return rc;
}

/* ------------------------------------------------------------------ */
/* KWAJ method 4 writer: raw DEFLATE with fixed Huffman codes.        */
static const unsigned short C_LEN_BASE[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const unsigned char C_LEN_EXTRA[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const unsigned short C_DIST_BASE[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
    6145, 8193, 12289, 16385, 24577
};
static const unsigned char C_DIST_EXTRA[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static const unsigned char C_CLORDER[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* dynamic-Huffman DEFLATE token with per-block code tables */
static int DeflateToken(BitWriter *bw, LzToken *t,
                        unsigned char *ll, unsigned int *lc,
                        unsigned char *dl, unsigned int *dc)
{
    if (!t->match)
        return BwCode(bw, lc[t->lit], ll[t->lit]);
    else
    {
        unsigned int len = t->len, dist = t->dist, sym;
        int i, nb, extra;
        for (i = 28; i >= 0; i--)
            if (len >= C_LEN_BASE[i]) break;
        if (i < 0) return -1;
        sym = 257u + (unsigned int)i;
        if (BwCode(bw, lc[sym], ll[sym])) return -1;
        extra = (int)(len - C_LEN_BASE[i]);
        nb = C_LEN_EXTRA[i];
        if (nb && BwLsb(bw, (unsigned int)extra, nb)) return -1;

        for (i = 29; i >= 0; i--)
            if (dist >= C_DIST_BASE[i]) break;
        if (i < 0) return -1;
        if (BwCode(bw, dc[i], dl[i])) return -1;
        nb = C_DIST_EXTRA[i];
        extra = (int)(dist - C_DIST_BASE[i]);
        if (nb && BwLsb(bw, (unsigned int)extra, nb)) return -1;
        return 0;
    }
}

typedef struct
{
    int sym, extra, nbits;
} ClSym;

/* Turn a list of code lengths into the DEFLATE code-length alphabet,
 * using repeat codes 16/17/18 for long runs. */
static int MakeClSeq(unsigned char *lens, int n, ClSym *seq)
{
    int i = 0, k = 0, prev = -1;
    while (i < n)
    {
        int v = lens[i], cnt = 1, rem;
        while (i + cnt < n && lens[i + cnt] == v) cnt++;
        rem = cnt;
        if (v == 0)
        {
            while (rem >= 11)
            {
                int t = rem > 138 ? 138 : rem;
                seq[k].sym = 18; seq[k].extra = t - 11; seq[k++].nbits = 7;
                rem -= t;
            }
            while (rem >= 3)
            {
                int t = rem > 10 ? 10 : rem;
                seq[k].sym = 17; seq[k].extra = t - 3; seq[k++].nbits = 3;
                rem -= t;
            }
        }
        else if (v == prev && rem >= 3)
        {
            while (rem >= 3)
            {
                int t = rem > 6 ? 6 : rem;
                seq[k].sym = 16; seq[k].extra = t - 3; seq[k++].nbits = 2;
                rem -= t;
            }
        }
        while (rem--)
        {
            seq[k].sym = v; seq[k].extra = 0; seq[k++].nbits = 0;
        }
        prev = v;
        i += cnt;
    }
    return k;
}

static int WriteMszipBlock(BitWriter *bw, LzToken *tok,
                           unsigned long from, unsigned long to)
{
    unsigned long lf[288], df[32], clf[19];
    unsigned char ll[288], dl[32], cll[19];
    unsigned int lc[288], dc[32], clc[19];
    unsigned long i;
    int t, used_dist = 0;

    for (i = 0; i < 288; i++) lf[i] = 0;
    for (i = 0; i < 32; i++) df[i] = 0;
    lf[256] = 1;                       /* end-of-block */
    for (i = from; i < to; i++)
    {
        LzToken *p = &tok[i];
        if (!p->match)
            lf[p->lit]++;
        else
        {
            int k;
            for (k = 28; k >= 0; k--)
                if (p->len >= C_LEN_BASE[k]) break;
            if (k < 0) return -1;
            lf[257 + k]++;
            for (k = 29; k >= 0; k--)
                if (p->dist >= C_DIST_BASE[k]) break;
            if (k < 0) return -1;
            df[k]++;
            used_dist = 1;
        }
    }
    if (!used_dist) df[0] = 1;         /* keep one distance code */
    BuildTree(lf, 286, ll, lc);
    BuildTree(df, 30, dl, dc);

    {
        ClSym litseq[400], distseq[400];
        int nlit = MakeClSeq(ll, 286, litseq);
        int ndist = MakeClSeq(dl, 30, distseq);

        for (i = 0; i < 19; i++) clf[i] = 0;
        for (i = 0; i < (unsigned long)nlit; i++) clf[litseq[i].sym]++;
        for (i = 0; i < (unsigned long)ndist; i++) clf[distseq[i].sym]++;
        BuildTree(clf, 19, cll, clc);

        if (BwBit(bw, 1)) return -1;       /* BFINAL */
        if (BwLsb(bw, 2u, 2)) return -1;   /* BTYPE = 10, dynamic */
        if (BwLsb(bw, 29u, 5)) return -1;  /* HLIT  = 286 */
        if (BwLsb(bw, 29u, 5)) return -1;  /* HDIST = 30  */
        if (BwLsb(bw, 15u, 4)) return -1;  /* HCLEN = 19  */
        for (t = 0; t < 19; t++)
            if (BwLsb(bw, cll[C_CLORDER[t]], 3)) return -1;
        for (i = 0; i < (unsigned long)nlit; i++)
        {
            if (BwCode(bw, clc[litseq[i].sym], cll[litseq[i].sym])) return -1;
            if (litseq[i].nbits &&
                BwLsb(bw, (unsigned int)litseq[i].extra, litseq[i].nbits))
                return -1;
        }
        for (i = 0; i < (unsigned long)ndist; i++)
        {
            if (BwCode(bw, clc[distseq[i].sym], cll[distseq[i].sym])) return -1;
            if (distseq[i].nbits &&
                BwLsb(bw, (unsigned int)distseq[i].extra, distseq[i].nbits))
                return -1;
        }
    }
    for (i = from; i < to; i++)
        if (DeflateToken(bw, &tok[i], ll, lc, dl, dc)) return -1;
    return BwCode(bw, lc[256], ll[256]);
}

static int WriteMszip(LzToken *tok, unsigned long n, FILE *out)
{
    unsigned long i = 0;
    unsigned long bytes = 0;
    unsigned long from = 0;
    int rc = -1;

    for (;;)
    {
        BitWriter bw;
        unsigned long step;
        unsigned int clen;

        while (i < n && bytes < C_MSZIP_BLOCK)
        {
            bytes += tok[i].len;
            i++;
        }
        if (!BwInit(&bw)) return -1;
        if (WriteMszipBlock(&bw, tok, from, i))
        {
            BwFree(&bw);
            return -1;
        }
        if (BwFlush(&bw))
        {
            BwFree(&bw);
            return -1;
        }
        if (bw.len + 2u > 0xFFFFu)
        {
            BwFree(&bw);
            return -1;
        }
        clen = (unsigned int)bw.len + 2u;
        if (fputc((int)(clen & 0xFF), out) == EOF ||
            fputc((int)((clen >> 8) & 0xFF), out) == EOF ||
            fputc('C', out) == EOF || fputc('K', out) == EOF ||
            fwrite(bw.buf, 1, bw.len, out) != bw.len)
        {
            BwFree(&bw);
            return -1;
        }
        BwFree(&bw);
        step = i;
        if (i >= n)
        {
            (void)step;
            break;
        }
        from = i;
        bytes = 0;
    }
    if (fputc(0, out) == EOF || fputc(0, out) == EOF)
        return -1;
    rc = 0;
    return rc;
}

/* ------------------------------------------------------------------ */
static void Put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static void Put16(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static int CompressFile(const char *src, const char *dst,
                        int algo, int no_length, int quiet, int no_rename,
                        unsigned long *insize_out, unsigned long *outsize_out)
{
    FILE *in, *out;
    unsigned char *data;
    unsigned long got, insize, outsize;
    long end;
    LzToken *tok;
    unsigned long ntok;
    int winsize, start, minlen, maxlen;
    int rc = 1;

    in = fopen(src, "rb");
    if (!in)
    {
        printf("   Error -- cannot open input file:  %s\n", src);
        return 1;
    }
    fseek(in, 0, SEEK_END);
    end = ftell(in);
    if (end < 0) { fclose(in); return 1; }
    insize = (unsigned long)end;
    rewind(in);
    data = (unsigned char *)malloc((size_t)insize + 1);
    if (!data) { fclose(in); return 1; }
    got = fread(data, 1, (size_t)insize, in);
    fclose(in);
    if (got != insize) { free(data); return 1; }

    if (algo == 1)      { winsize = C_WINDOW_4096; start = C_START_EXPAND;
                          minlen = 3; maxlen = 18; }
    else if (algo == 5) { winsize = C_WINDOW_4096; start = C_START_KWAJ;
                          minlen = 3; maxlen = 18; }
    else if (algo == 2) { winsize = C_WINDOW_4096; start = C_START_KWAJ;
                          minlen = 3; maxlen = 18; }
    else if (algo == 3) { winsize = C_WINDOW_4096; start = C_START_KWAJ;
                          minlen = 3; maxlen = 17; }
    else                { winsize = 32768; start = 0;
                          minlen = 3; maxlen = 258; }

    if (LzParse(data, insize, winsize, 0x20, start,
                minlen, maxlen, winsize, &tok, &ntok))
    {
        free(data);
        return 1;
    }

    out = fopen(dst, "wb");
    if (!out)
    {
        printf("   Error -- cannot open output file:  %s\n", dst);
        free(tok); free(data);
        return 1;
    }

    if (algo == 1 || algo == 5)
    {
        unsigned char hdr[14];
        char missing = 0;
        /* replace the last extension character with an underscore */
        {
            const char *p = src, *last = src;
            for (; *p; p++) if (*p == '\\' || *p == '/') last = p + 1;
            {
                const char *dot = strrchr(last, '.');
                if (dot && dot[1] && dot[2] == 0) missing = dot[1];
                else if (*last) missing = last[strlen(last) - 1];
            }
        }
        memcpy(hdr, C_SIG_SZDD, 8);
        hdr[8] = (algo == 5) ? 'B' : 'A';
        hdr[9] = (unsigned char)missing;
        Put32(hdr + 10, insize);
        if (fwrite(hdr, 1, 14, out) != 14) goto out_fail;
        if (WriteLzss(tok, ntok, out)) goto out_fail;
    }
    else
    {
        unsigned char hdr[18];
        unsigned int flags = 0, hsize = 14;
        int method = (algo == 2) ? 2 : (algo == 3) ? 3 : 4;
        memcpy(hdr, C_SIG_KWAJ, 8);
        Put16(hdr + 8, (unsigned int)method);
        if (algo != 4 && !no_length)
        {
            flags |= 0x01;
            hsize = 18;
            Put32(hdr + 14, insize);
        }
        Put16(hdr + 10, hsize);
        Put16(hdr + 12, flags);
        if (fwrite(hdr, 1, hsize, out) != hsize) goto out_fail;

        if (algo == 2)
        {
            if (WriteLzss(tok, ntok, out)) goto out_fail;
        }
        else if (algo == 3)
        {
            if (WriteHuffman(tok, ntok, out)) goto out_fail;
        }
        else if (algo == 4)
        {
            if (WriteMszip(tok, ntok, out)) goto out_fail;
        }
    }

    fclose(out);
    {
        FILE *check = fopen(dst, "rb");
        if (check)
        {
            fseek(check, 0, SEEK_END);
            end = ftell(check);
            outsize = (unsigned long)(end < 0 ? 0 : end);
            fclose(check);
        }
        else
            outsize = 0;
    }

    (void)quiet;
    (void)no_rename;
    if (insize_out) *insize_out = insize;
    if (outsize_out) *outsize_out = outsize;
    rc = 0;
    goto done;

out_fail:
    fclose(out);
done:
    free(tok);
    free(data);
    return rc;
}

static void PrintStats(unsigned long insize, unsigned long outsize,
                       const char *dst)
{
    long diff;
    int pct;
    printf("     %ld bytes compresses to %ld bytes,  \n",
           (long)insize, (long)outsize);
    diff = (long)insize - (long)outsize;
    pct = insize ? (int)((diff * 100L) / (long)insize) : 0;
    if (pct <= 0)
        printf("   No Savings\n");
    else
        printf("   Savings: %d percent\n", pct);
    if (dst)
        printf("        Wrote %6ld bytes to output file '%s'\n",
               (long)outsize, dst);
}

static void Usage(void)
{
    printf("Usage: compress [-aAlg -bceflq$ -sSize -tText -zSize] srcArg [destArg]\n");
    printf("   -a -- Choices for compression algorithm are:  [default = 3]\n");
    printf("      1 - the old SZDD format.\n");
    printf("      2 - the Steven Zeck algorithm (LZSS).\n");
    printf("      3 - Jeff Johnson's algorithm (LZSS + Huffman).\n");
    printf("      4 - KWAJ method 4 (MS-ZIP, extension).\n");
    printf("      5 - SZDD type B (LZSS, window start 4096-18).\n");
    printf("   -l will NOT include each file length in the header.\n");
    printf("   -q will compute compressed file lengths (no output).\n");
    printf("   -$ will turn OFF underscore renaming.\n");
}

static int IsDir(const char *p)
{
    struct _stat st;
    size_t n;
    if (_stat(p, &st) == 0)
        return (st.st_mode & _S_IFDIR) != 0;
    n = strlen(p);
    return n > 0 && (p[n - 1] == '\\' || p[n - 1] == '/');
}

int main(int argc, char **argv)
{
    int i, algo = 3, no_length = 0, quiet = 0, no_rename = 0;
    const char *src = NULL, *dest = NULL;
    char out[_MAX_PATH];
    char name[_MAX_PATH];

    printf("Microsoft (R) Compression Utility - Version 1.11\n");
    printf("Copyright (c) Microsoft Corp 1989 - 1992.  All rights reserved.\n\n");

    for (i = 1; i < argc; i++)
    {
        const char *a = argv[i];
        if (a[0] != '-' && a[0] != '/')
            break;
        if (a[1] == 'a')
        {
            algo = atoi(a + 2);
            if (algo < 1 || algo > 5) { Usage(); return 1; }
        }
        else if (a[1] == 'l') no_length = 1;
        else if (a[1] == 'q') quiet = 1;
        else if (a[1] == '$') no_rename = 1;
        else if (a[1] == 'f' || a[1] == 'b' || a[1] == 'c' || a[1] == 'e')
            ; /* accepted, no effect on our headers */
        else
        {
            printf("Unrecognized switch -%c.\n", a[1]);
            Usage();
            return 1;
        }
    }
    if (i >= argc) { Usage(); return 1; }
    src = argv[i++];
    if (i < argc) dest = argv[i];

    if (dest)
    {
        if (IsDir(dest))
        {
            const char *base = src, *p;
            for (p = src; *p; p++)
                if (*p == '\\' || *p == '/') base = p + 1;
            strcpy(name, base);
            {
                char *dot = strrchr(name, '.');
                if (dot && dot[1])
                    dot[1] = '_';
                else if (name[0])
                    name[strlen(name) - 1] = '_';
            }
            strcpy(out, dest);
            if (out[strlen(out) - 1] != '\\') strcat(out, "\\");
            strcat(out, name);
        }
        else
            strcpy(out, dest);
    }
    else
    {
        const char *p, *base = src;
        for (p = src; *p; p++)
            if (*p == '\\' || *p == '/') base = p + 1;
        strcpy(name, base);
        {
            char *dot = strrchr(name, '.');
            if (dot && dot[1] && no_rename)
                strcat(name, "_");
            else if (dot && dot[1])
                dot[1] = '_';
            else if (name[0])
                name[strlen(name) - 1] = '_';
        }
        strcpy(out, name);
    }

    {
        unsigned long insize = 0, outsize = 0;
        int rc = CompressFile(src, out, algo, no_length,
                              quiet, no_rename, &insize, &outsize);
        if (rc == 0 && !quiet)
            PrintStats(insize, outsize, no_rename ? NULL : out);
        return rc;
    }
}
