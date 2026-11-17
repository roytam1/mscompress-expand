#ifndef LZENCODE_H
#define LZENCODE_H

/* Shared LZ token parser for the COMPRESS.EXE reconstruction.
 * It produces a token stream (literals and matches) that the individual
 * format writers turn into LZSS, KWAJ Huffman or MS-ZIP data. */

typedef struct
{
    int match;                 /* 0 = literal, 1 = match */
    unsigned char lit;         /* literal byte */
    unsigned int pos;          /* match start, absolute position in window */
    unsigned int dist;         /* match distance, 1..winsize */
    unsigned int len;          /* match length */
} LzToken;

/* Parse "insize" bytes into tokens.  The window is "winsize" bytes wide,
 * prefilled with "fill", and the write pointer starts at "startpos".
 * Matches are between minlen and maxlen bytes long and at most maxdist
 * bytes away.  Returns 0 on success, -1 on error.  *tokens is malloc'ed. */
int LzParse(const unsigned char *in, unsigned long insize,
            int winsize, int fill, int startpos,
            int minlen, int maxlen, int maxdist,
            LzToken **tokens, unsigned long *ntokens);

#endif
