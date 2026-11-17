/*
 * lzencode.c - LZ token parser shared by the SZDD, KWAJ-2, KWAJ-3 and
 * KWAJ-4 writers.  The window and match rules are the exact inverse of
 * the decoders in mscompress.c.
 */

#include <stdlib.h>
#include <string.h>
#include "lzencode.h"

#define LZ_HASH_BITS 16
#define LZ_HASH_SIZE (1 << LZ_HASH_BITS)
#define LZ_MAX_CHAIN 256

static unsigned int LzHash(const unsigned char *p)
{
    unsigned int h;
    h = ((unsigned int)p[0] << 8) ^ ((unsigned int)p[1] << 4) ^ p[2];
    return h & (LZ_HASH_SIZE - 1u);
}

int LzParse(const unsigned char *in, unsigned long insize,
            int winsize, int fill, int startpos,
            int minlen, int maxlen, int maxdist,
            LzToken **tokens, unsigned long *ntokens)
{
    unsigned char *win;
    int *head, *prev, *wpos;
    unsigned int mask;
    unsigned int pos;
    unsigned long i, cap, n = 0;
    LzToken *tok;
    int ok = 1;

    if (winsize < 16 || (winsize & (winsize - 1)) != 0 ||
        minlen < 2 || maxlen < minlen || maxdist < 1)
        return -1;

    win = (unsigned char *)malloc((size_t)winsize);
    head = (int *)malloc(LZ_HASH_SIZE * sizeof(int));
    prev = (int *)malloc((size_t)insize * sizeof(int) + sizeof(int));
    wpos = (int *)malloc((size_t)insize * sizeof(int) + sizeof(int));
    cap = insize * 2 + 16;
    tok = (LzToken *)malloc((size_t)cap * sizeof(LzToken));
    if (!win || !head || !prev || !wpos || !tok)
    {
        free(win); free(head); free(prev); free(wpos); free(tok);
        return -1;
    }

    memset(win, fill, (size_t)winsize);
    for (i = 0; i < LZ_HASH_SIZE; i++)
        head[i] = -1;
    mask = (unsigned int)(winsize - 1);
    pos = (unsigned int)startpos & mask;

#define LZ_INSERT(idx, wp)                                            \
    do {                                                              \
        if ((idx) + 2 < insize)                                       \
        {                                                             \
            unsigned int h_ = LzHash(in + (idx));                      \
            prev[(idx)] = head[h_];                                    \
            head[h_] = (int)(idx);                                     \
            wpos[(idx)] = (int)(wp);                                   \
        }                                                             \
        else                                                          \
            prev[(idx)] = -1;                                          \
    } while (0)

    i = 0;
    while (i < insize)
    {
        unsigned int bestlen = 0, bestdist = 0, bestpos = 0;
        if (i + 3 <= insize)
        {
            unsigned int h = LzHash(in + i);
            int cand = head[h];
            int tries = 0;
            while (cand >= 0 && tries < LZ_MAX_CHAIN)
            {
                unsigned int cp = (unsigned int)wpos[cand] & mask;
                unsigned int d = (pos - cp) & mask;
                int cmp = 0;
                if (d >= 1 && d <= (unsigned int)maxdist)
                {
                    unsigned int k, limit;
                    limit = (unsigned int)maxlen;
                    if (limit > insize - i)
                        limit = (unsigned int)(insize - i);
                    for (k = 0; k < limit; k++)
                    {
                        /* An overlapping copy reads back bytes written by
                         * this same match: after the first "d" bytes the
                         * output repeats with period "d". */
                        unsigned char want = (k >= d) ? in[i + k - d]
                                                      : win[(cp + k) & mask];
                        if (want != in[i + k])
                            break;
                        cmp++;
                    }
                }
                if ((unsigned int)cmp > bestlen)
                {
                    bestlen = (unsigned int)cmp;
                    bestdist = d;
                    bestpos = cp;
                    if (bestlen >= (unsigned int)maxlen)
                        break;
                }
                cand = prev[cand];
                tries++;
            }
        }

        if (bestlen >= (unsigned int)minlen)
        {
            unsigned int k;
            if (n >= cap)
            {
                LzToken *tmp;
                cap *= 2;
                tmp = (LzToken *)realloc(tok, (size_t)cap * sizeof(LzToken));
                if (!tmp) { ok = 0; break; }
                tok = tmp;
            }
            tok[n].match = 1;
            tok[n].lit = 0;
            tok[n].pos = bestpos;
            tok[n].dist = bestdist;
            tok[n].len = bestlen;
            n++;
            for (k = 0; k < bestlen; k++)
            {
                win[pos] = in[i + k];
                LZ_INSERT(i + k, pos);
                pos = (pos + 1) & mask;
            }
            i += bestlen;
        }
        else
        {
            if (n >= cap)
            {
                LzToken *tmp;
                cap *= 2;
                tmp = (LzToken *)realloc(tok, (size_t)cap * sizeof(LzToken));
                if (!tmp) { ok = 0; break; }
                tok = tmp;
            }
            tok[n].match = 0;
            tok[n].lit = in[i];
            tok[n].pos = 0;
            tok[n].dist = 0;
            tok[n].len = 1;
            n++;
            win[pos] = in[i];
            LZ_INSERT(i, pos);
            pos = (pos + 1) & mask;
            i++;
        }
    }

#undef LZ_INSERT

    if (!ok)
    {
        free(win); free(head); free(prev); free(wpos); free(tok);
        return -1;
    }
    free(win); free(head); free(prev); free(wpos);
    *tokens = tok;
    *ntokens = n;
    return 0;
}
