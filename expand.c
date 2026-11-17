/*
 * expand.c - functional C89 reconstruction of Microsoft's EXPAND.EXE
 * (File Expansion Utility version 2.10) for the SZDD and KWAJ formats.
 *
 * Build with MSVC6 (see BUILD.BAT).  Unlike the original 16 bit program
 * this is a 32 bit console application, but the command line, messages
 * and file formats are compatible.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <io.h>
#include <direct.h>
#include <sys/stat.h>
#include "mscompress.h"

static int g_total_files = 0;
static unsigned long g_total_in = 0;
static unsigned long g_total_out = 0;

static void Banner(void)
{
    printf("Microsoft (R) File Expansion Utility  Version 2.10\n");
    printf("Copyright (C) Microsoft Corp 1990-1993.  All rights reserved.\n\n");
}

static void Usage(void)
{
    printf("Expands one or more compressed files.\n\n");
    printf("EXPAND [-r] Source Destination\n");
    printf("EXPAND -r Source [Destination]\n\n");
    printf("   -r          Automatically rename expanded files. Only valid for files\n");
    printf("               compressed with -r switch.\n");
    printf("   Source      Source file specification. Source may be multiple file\n");
    printf("               specifications. Wildcards may be used.\n");
    printf("   Destination Destination file / path specification. Destination may be a\n");
    printf("               directory. If Source is multiple files and -r is not specified,\n");
    printf("               Destination must be a directory. Wildcards may not be used.\n");
}

static int IsDir(const char *path)
{
    struct _stat st;
    size_t n;
    if (_stat(path, &st) == 0)
        return (st.st_mode & _S_IFDIR) != 0;
    n = strlen(path);
    return n > 0 && (path[n - 1] == '\\' || path[n - 1] == '/');
}

static int HasWild(const char *s)
{
    return strchr(s, '*') != NULL || strchr(s, '?') != NULL;
}

static void StripPath(const char *path, char *base, int sz)
{
    const char *p, *last = path;
    for (p = path; *p; p++)
        if (*p == '\\' || *p == '/')
            last = p + 1;
    if ((int)strlen(last) >= sz)
    {
        /* avoid overflow: copy the tail if the name is very long */
        int len = (int)strlen(last);
        last += len - (sz - 1);
    }
    strcpy(base, last);
}

static int SameFile(const char *a, const char *b)
{
    char fa[_MAX_PATH], fb[_MAX_PATH];
    char *pa, *pb;
    if (_fullpath(fa, a, sizeof(fa)) == NULL ||
        _fullpath(fb, b, sizeof(fb)) == NULL)
        return 0;
    pa = fa;
    pb = fb;
    for (; *pa; pa++, pb++)
        if (tolower((unsigned char)*pa) != tolower((unsigned char)*pb))
            return 0;
    return *pb == 0;
}

/* Work out the expanded name for a source file. */
static void DerivedName(const char *src, const char *destdir,
                        char missingchar, char *out, int outsz)
{
    char name[_MAX_PATH];
    char dir[_MAX_PATH];

    MsCompressMakeName(src, missingchar, name, (int)sizeof(name));
    if (destdir && *destdir)
    {
        strcpy(dir, destdir);
        if (dir[0] && dir[strlen(dir) - 1] != '\\' &&
            dir[strlen(dir) - 1] != '/')
            strcat(dir, "\\");
        strcpy(out, dir);
        strcat(out, name);
    }
    else
        strcpy(out, name);
}

static int ExpandOne(const char *src, const char *dst, int autoname, int quiet)
{
    FILE *in, *out;
    MsCompressHeader h;
    unsigned long insize, outsize;
    long pos, end;
    int rc, copied = 0, pct;

    in = fopen(src, "rb");
    if (in == NULL)
    {
        printf("Can't open input file: %s.\n", src);
        return 1;
    }
    if (SameFile(src, dst))
    {
        printf("Cannot expand a file onto itself: %s.\n", src);
        fclose(in);
        return 1;
    }
    fseek(in, 0, SEEK_END);
    end = ftell(in);
    insize = (unsigned long)(end < 0 ? 0 : end);
    rewind(in);

    rc = MsCompressReadHeader(in, &h);
    if (rc == MSCOMPRESS_ERR_SIGNATURE)
        copied = 1;
    else if (rc != MSCOMPRESS_OK)
    {
        printf("Error in compressed input file format: %s.\n", src);
        fclose(in);
        return 1;
    }
    pos = ftell(in);
    (void)pos;

    out = fopen(dst, "wb");
    if (out == NULL)
    {
        printf("Can't open output file: %s.\n", dst);
        fclose(in);
        return 1;
    }

    if (!quiet)
        printf(copied ? "Copying %s to %s.\n" : "Expanding %s to %s.\n",
               src, dst);

    if (copied)
    {
        /* not a compressed file: copy it through unchanged */
        char buf[8192];
        size_t got;
        rewind(in);
        while ((got = fread(buf, 1, sizeof(buf), in)) > 0)
            if (fwrite(buf, 1, got, out) != got)
            {
                printf("Out of disk space writing output file: %s.\n", dst);
                fclose(in);
                fclose(out);
                return 1;
            }
        outsize = insize;
        rc = 0;
    }
    else
    {
        rc = MsCompressDecode(in, out, &h);
        fseek(out, 0, SEEK_END);
        end = ftell(out);
        outsize = (unsigned long)(end < 0 ? 0 : end);
        if (rc == MSCOMPRESS_ERR_WRITE)
        {
            printf("Out of disk space writing output file: %s.\n", dst);
            rc = 1;
        }
        else if (rc == MSCOMPRESS_ERR_UNSUPPORTED)
        {
            printf("Unrecognized compression scheme: %s.\n", src);
            rc = 1;
        }
        else if (rc != MSCOMPRESS_OK)
        {
            printf("Expanding %s to %s failed.\n", src, dst);
            rc = 1;
        }
        else
            rc = 0;
    }
    fclose(in);
    fclose(out);

    if (rc)
        return 1;

    if (!quiet)
    {
        if (copied)
            printf("%s: %ld bytes copied.\n", dst, (long)insize);
        else
        {
            if (insize)
            {
                long diff = (long)outsize - (long)insize;
                pct = (int)((diff * 100L) / (long)insize);
                if (pct < 0) pct = -pct;
                printf("%s: %ld bytes expanded to %ld bytes, %d%% increase.\n",
                       dst, (long)insize, (long)outsize, pct);
            }
            else
                printf("%s: %ld bytes expanded to %ld bytes.\n",
                       dst, (long)insize, (long)outsize);
        }
    }

    g_total_files++;
    g_total_in += insize;
    g_total_out += outsize;
    return 0;
}

static void Totals(void)
{
    long diff;
    int pct;
    if (g_total_files < 2)
        return;
    diff = (long)g_total_out - (long)g_total_in;
    pct = g_total_in ? (int)((diff * 100L) / (long)g_total_in) : 0;
    if (pct < 0) pct = -pct;
    printf("Total increase: %d files, %ld bytes expanded to %ld bytes, "
           "%d%% increase.\n",
           g_total_files, (long)g_total_in, (long)g_total_out, pct);
}

static int Interactive(int autoname)
{
    char src[_MAX_PATH], dst[_MAX_PATH];
    char line[1024];
    int n;

    printf("Compressed file: \n");
    printf("Enter the location and name of the compressed file to expand.\n");
    printf("(Example: A:\\HIMEM.SY_)\n");
    if (fgets(line, sizeof(line), stdin) == NULL)
        return 1;
    n = (int)strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = 0;
    strcpy(src, line);

    if (autoname)
    {
        FILE *in = fopen(src, "rb");
        MsCompressHeader h;
        char name[_MAX_PATH];
        printf("Destination: \n");
        printf("Enter the location for the expanded files.\n");
        printf("(Example: C:\\WINDOWS)\n");
        if (fgets(line, sizeof(line), stdin) == NULL)
        {
            if (in) fclose(in);
            return 1;
        }
        n = (int)strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        if (in && MsCompressReadHeader(in, &h) == MSCOMPRESS_OK)
            DerivedName(src, line, h.missingchar, name, (int)sizeof(name));
        else
            strcpy(name, line);
        if (in) fclose(in);
        return ExpandOne(src, name, 1, 0);
    }

    printf("Expanded file: \n");
    printf("Enter the location and/or name for the expanded file.\n");
    printf("(Example: C:\\WINDOWS\\HIMEM.SYS)\n");
    if (fgets(line, sizeof(line), stdin) == NULL)
        return 1;
    n = (int)strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
        line[--n] = 0;
    strcpy(dst, line);
    return ExpandOne(src, dst, 0, 0);
}

int main(int argc, char **argv)
{
    int i, nargs, nsrc, autoname = 0, errors = 0;
    const char *args[256];
    const char *dest = NULL;

    Banner();

    for (i = 1; i < argc; i++)
    {
        if (argv[i][0] == '-' || argv[i][0] == '/')
        {
            if (tolower((unsigned char)argv[i][1]) == 'r' && argv[i][2] == 0)
                autoname = 1;
            else
            {
                printf("Unrecognized switch -%c.\n", argv[i][1]);
                Usage();
                return 1;
            }
        }
        else
            break;
    }

    nargs = 0;
    for (; i < argc && nargs < 256; i++)
        args[nargs++] = argv[i];

    if (nargs == 0)
        return Interactive(autoname);

    /* The final argument is the destination when there is more than one
     * (or when -r is used with an explicit destination). */
    nsrc = nargs;
    if (nargs >= 2)
    {
        dest = args[nargs - 1];
        nsrc = nargs - 1;
    }

    if (dest && HasWild(dest))
    {
        printf("Wildcards may not be used in the Destination.\n");
        return 1;
    }
    if (nsrc > 1 && dest && !IsDir(dest))
    {
        printf("Destination is not a directory: %s.\n", dest);
        return 1;
    }

    for (i = 0; i < nsrc; i++)
    {
        char src[_MAX_PATH];
        char out[_MAX_PATH];
        struct _finddata_t fd;
        long handle;

        if (!HasWild(args[i]))
        {
            char name[_MAX_PATH];
            char dir[_MAX_PATH];
            FILE *in;
            MsCompressHeader h;
            int known = 0;

            strcpy(src, args[i]);
            in = fopen(src, "rb");
            if (in != NULL)
            {
                if (MsCompressReadHeader(in, &h) == MSCOMPRESS_OK)
                {
                    known = 1;
                    MsCompressMakeName(src, h.missingchar,
                                       name, (int)sizeof(name));
                }
                fclose(in);
            }
            if (!known)
                strcpy(name, src);

            if (dest == NULL || autoname || IsDir(dest))
            {
                dir[0] = 0;
                if (dest && IsDir(dest))
                {
                    strcpy(dir, dest);
                    if (dir[strlen(dir) - 1] != '\\') strcat(dir, "\\");
                }
                strcpy(out, dir);
                strcat(out, name);
            }
            else
                strcpy(out, dest);
            errors += ExpandOne(src, out, autoname, 0);
            continue;
        }

        handle = _findfirst(args[i], &fd);
        if (handle == -1L)
        {
            printf("Can't open input file: %s.\n", args[i]);
            errors++;
            continue;
        }
        do
        {
            FILE *in;
            MsCompressHeader h;
            char name[_MAX_PATH];
            char dir[_MAX_PATH];
            int known = 0;

            strcpy(src, fd.name);
            in = fopen(src, "rb");
            if (in != NULL)
            {
                if (MsCompressReadHeader(in, &h) == MSCOMPRESS_OK)
                {
                    known = 1;
                    MsCompressMakeName(src, h.missingchar,
                                       name, (int)sizeof(name));
                }
                fclose(in);
            }
            if (!known)
                strcpy(name, src);

            dir[0] = 0;
            if (dest)
            {
                strcpy(dir, dest);
                if (dir[strlen(dir) - 1] != '\\') strcat(dir, "\\");
            }
            strcpy(out, dir);
            strcat(out, name);
            errors += ExpandOne(src, out, autoname, 0);
        } while (_findnext(handle, &fd) == 0);
        _findclose(handle);
    }

    Totals();
    if (errors)
        return 1;
    return 0;
}
