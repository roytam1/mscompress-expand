#ifndef MSCOMPRESS_H
#define MSCOMPRESS_H

#include <stdio.h>

/* C89 decoders for the COMPRESS.EXE / EXPAND.EXE SZDD and KWAJ formats. */

enum ms_compress_format
{
    MSCOMPRESS_NONE = 0,
    MSCOMPRESS_SZDD,
    MSCOMPRESS_SZDD_QBASIC,
    MSCOMPRESS_KWAJ
};

enum ms_compress_result
{
    MSCOMPRESS_OK = 0,
    MSCOMPRESS_ERR_SIGNATURE,
    MSCOMPRESS_ERR_UNSUPPORTED,
    MSCOMPRESS_ERR_DATA,
    MSCOMPRESS_ERR_READ,
    MSCOMPRESS_ERR_WRITE
};

typedef struct
{
    int format;
    int method;
    unsigned long origsize;
    unsigned long dataoffset;
    char missingchar;
} MsCompressHeader;

int MsCompressReadHeader(FILE *in, MsCompressHeader *h);
int MsCompressDecode(FILE *in, FILE *out, const MsCompressHeader *h);
int MsCompressMakeName(const char *arcname, char missingchar,
                       char *out, int outsz);
const char *MsCompressModeName(const MsCompressHeader *h);

#endif
