#ifndef MSINFLATE_H
#define MSINFLATE_H

#include <stdio.h>

/*
 * Small raw-DEFLATE (RFC 1951) decoder used by the KWAJ method 4 (MS-ZIP)
 * reader.  Each MS-ZIP block is an independent raw deflate stream, but the
 * 32 KiB history window is shared between the blocks of a folder, so the
 * window lives in this structure and survives between MsInflateStream calls.
 */

#define MSI_WIN_SIZE 32768u
#define MSI_WIN_MASK (MSI_WIN_SIZE - 1u)

typedef struct
{
    unsigned char win[MSI_WIN_SIZE];
    unsigned int pos;          /* next write position in the history window */
    unsigned long bitbuf;      /* bit buffer, bits consumed low end first */
    int bitcount;
    const unsigned char *src;
    unsigned long srclen;
    unsigned long srcpos;
} MsInflate;

void MsInflateInit(MsInflate *z);
int MsInflateStream(MsInflate *z, const unsigned char *src,
                    unsigned long srclen, FILE *out, unsigned long *written);

#endif
