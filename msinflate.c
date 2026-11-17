/*
 * msinflate.c - a compact raw DEFLATE decoder for KWAJ method 4 (MS-ZIP).
 *
 * Written in C89 so that it builds with Microsoft Visual C++ 6.0 without
 * any external compression library.  The history window is kept in the
 * caller's MsInflate structure, which lets consecutive MS-ZIP blocks refer
 * back to data decompressed from earlier blocks.
 */

#include <stdio.h>
#include <string.h>
#include "msinflate.h"

#define MSI_MAXBITS 16

typedef struct
{
    int count[MSI_MAXBITS + 1];
    int firstcode[MSI_MAXBITS + 1];
    int firstindex[MSI_MAXBITS + 1];
    int symbol[288];
} MsHuff;

static const unsigned short msi_lenbase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const unsigned char msi_lenextra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const unsigned short msi_distbase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
    6145, 8193, 12289, 16385, 24577
};
static const unsigned char msi_distextra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
static const unsigned char msi_clorder[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

void MsInflateInit(MsInflate *z)
{
    memset(z->win, 0, sizeof(z->win));
    z->pos = 0;
    z->bitbuf = 0;
    z->bitcount = 0;
    z->src = NULL;
    z->srclen = 0;
    z->srcpos = 0;
}

static int msi_bit(MsInflate *z)
{
    int bit;
    if (z->bitcount == 0)
    {
        if (z->srcpos >= z->srclen)
            return -1;
        z->bitbuf = (unsigned long)z->src[z->srcpos++];
        z->bitcount = 8;
    }
    bit = (int)(z->bitbuf & 1UL);
    z->bitbuf >>= 1;
    z->bitcount--;
    return bit;
}

static int msi_bits(MsInflate *z, int n)
{
    int v = 0, i, b;
    for (i = 0; i < n; i++)
    {
        b = msi_bit(z);
        if (b < 0)
            return -1;
        v |= (b << i);
    }
    return v;
}

static void msi_build(MsHuff *h, const unsigned char *lengths, int n)
{
    int i, l, code, index;

    for (i = 0; i <= MSI_MAXBITS; i++)
        h->count[i] = 0;
    for (i = 0; i < n; i++)
    {
        int l = lengths[i];
        if (l > MSI_MAXBITS)
            l = MSI_MAXBITS;
        h->count[l]++;
    }
    code = 0;
    for (l = 1; l <= MSI_MAXBITS; l++)
    {
        h->firstcode[l] = code;
        code = (code + h->count[l]) << 1;
    }
    index = 0;
    for (l = 1; l <= MSI_MAXBITS; l++)
    {
        h->firstindex[l] = index;
        for (i = 0; i < n; i++)
            if ((int)lengths[i] == l)
                h->symbol[index++] = i;
    }
}

static int msi_decode(MsInflate *z, const MsHuff *h)
{
    int code = 0, len = 0;
    for (;;)
    {
        int b = msi_bit(z);
        if (b < 0)
            return -1;
        code = (code << 1) | b;
        len++;
        if (len > MSI_MAXBITS)
            return -1;
        if (h->count[len])
        {
            int index = code - h->firstcode[len];
            if (index >= 0 && index < h->count[len])
                return h->symbol[h->firstindex[len] + index];
        }
    }
}

static int msi_put(MsInflate *z, unsigned char ch, FILE *out,
                   unsigned long *written)
{
    z->win[z->pos] = ch;
    z->pos = (z->pos + 1u) & MSI_WIN_MASK;
    if (fputc((int)ch, out) == EOF)
        return -1;
    (*written)++;
    return 0;
}

static int msi_fixed(MsHuff *lit, MsHuff *dist)
{
    unsigned char len[288], dl[32];
    int i;
    for (i = 0; i < 144; i++) len[i] = 8;
    for (; i < 256; i++) len[i] = 9;
    for (; i < 280; i++) len[i] = 7;
    for (; i < 288; i++) len[i] = 8;
    for (i = 0; i < 32; i++) dl[i] = 5;
    msi_build(lit, len, 288);
    msi_build(dist, dl, 32);
    return 0;
}

static int msi_dynamic(MsInflate *z, MsHuff *lit, MsHuff *dist)
{
    unsigned char cl[19], lengths[288 + 32];
    MsHuff cltree;
    int hlit, hdist, hclen, i, n, sym;
    hlit = msi_bits(z, 5);
    hdist = msi_bits(z, 5);
    hclen = msi_bits(z, 4);
    if (hlit < 0 || hdist < 0 || hclen < 0)
        return -1;
    hlit += 257;
    hdist += 1;
    hclen += 4;
    if (hlit > 288 || hdist > 32)
        return -1;

    for (i = 0; i < 19; i++)
        cl[i] = 0;
    for (i = 0; i < hclen; i++)
    {
        int v = msi_bits(z, 3);
        if (v < 0)
            return -1;
        cl[(int)msi_clorder[i]] = (unsigned char)v;
    }
    msi_build(&cltree, cl, 19);

    n = hlit + hdist;
    i = 0;
    while (i < n)
    {
        sym = msi_decode(z, &cltree);
        if (sym < 0)
            return -1;
        if (sym < 16)
        {
            lengths[i++] = (unsigned char)sym;
        }
        else if (sym == 16)
        {
            int extra, rep;
            if (i == 0)
                return -1;
            extra = msi_bits(z, 2);
            if (extra < 0)
                return -1;
            rep = 3 + extra;
            if (i + rep > n)
                return -1;
            while (rep-- > 0)
            {
                lengths[i] = lengths[i - 1];
                i++;
            }
        }
        else if (sym == 17)
        {
            int extra, rep;
            extra = msi_bits(z, 3);
            if (extra < 0)
                return -1;
            rep = 3 + extra;
            if (i + rep > n)
                return -1;
            while (rep-- > 0) { lengths[i++] = 0; }
        }
        else if (sym == 18)
        {
            int extra, rep;
            extra = msi_bits(z, 7);
            if (extra < 0)
                return -1;
            rep = 11 + extra;
            if (i + rep > n)
                return -1;
            while (rep-- > 0) { lengths[i++] = 0; }
        }
        else
            return -1;
    }
    msi_build(lit, lengths, hlit);
    msi_build(dist, lengths + hlit, hdist);
    return 0;
}

static int msi_stored(MsInflate *z, FILE *out, unsigned long *written)
{
    int c, len, nlen, i;
    /* skip to the next byte boundary */
    z->bitbuf = 0;
    z->bitcount = 0;
    c = msi_bits(z, 8);
    len = msi_bits(z, 8);
    if (c < 0 || len < 0)
        return -1;
    len |= (c << 8);
    c = msi_bits(z, 8);
    nlen = msi_bits(z, 8);
    if (c < 0 || nlen < 0)
        return -1;
    nlen |= (c << 8);
    if ((((unsigned int)len ^ 0xffffu) & 0xffffu) != (unsigned int)nlen)
        return -1;
    for (i = 0; i < len; i++)
    {
        int ch = msi_bits(z, 8);
        if (ch < 0 || msi_put(z, (unsigned char)ch, out, written))
            return -1;
    }
    return 0;
}

int MsInflateStream(MsInflate *z, const unsigned char *src,
                    unsigned long srclen, FILE *out, unsigned long *written)
{
    z->src = src;
    z->srclen = srclen;
    z->srcpos = 0;
    z->bitbuf = 0;
    z->bitcount = 0;

    for (;;)
    {
        MsHuff lit, dist;
        int last, type, sym, err;

        last = msi_bit(z);
        type = msi_bits(z, 2);
        if (last < 0 || type < 0)
            return -1;

        if (type == 0)
        {
            if (msi_stored(z, out, written))
                return -1;
        }
        else
        {
            if (type == 1)
                err = msi_fixed(&lit, &dist);
            else if (type == 2)
                err = msi_dynamic(z, &lit, &dist);
            else
                return -1;
            if (err)
                return -1;

            for (;;)
            {
                sym = msi_decode(z, &lit);
                if (sym < 0)
                    return -1;
                if (sym < 256)
                {
                    if (msi_put(z, (unsigned char)sym, out, written))
                        return -1;
                }
                else if (sym == 256)
                    break;
                else
                {
                    int length, distance, extra, i, d;
                    sym -= 257;
                    if (sym >= 29)
                        return -1;
                    length = (int)msi_lenbase[sym];
                    extra = msi_bits(z, (int)msi_lenextra[sym]);
                    if (extra < 0)
                        return -1;
                    length += extra;

                    sym = msi_decode(z, &dist);
                    if (sym < 0 || sym >= 30)
                        return -1;
                    distance = (int)msi_distbase[sym];
                    extra = msi_bits(z, (int)msi_distextra[sym]);
                    if (extra < 0)
                        return -1;
                    distance += extra;
                    if (distance <= 0 || distance > (int)MSI_WIN_SIZE)
                        return -1;
                    d = (int)((z->pos - (unsigned int)distance) & MSI_WIN_MASK);
                    for (i = 0; i < length; i++)
                    {
                        unsigned char ch = z->win[(unsigned int)d & MSI_WIN_MASK];
                        if (msi_put(z, ch, out, written))
                            return -1;
                        d = (d + 1) & (int)MSI_WIN_MASK;
                    }
                }
            }
        }
        if (last)
            break;
    }

    return 0;
}
