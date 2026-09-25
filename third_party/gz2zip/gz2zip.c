/*
  gz2zip.c version 1.0, 31 July 2018

  Copyright (C) 2018 Mark Adler

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Mark Adler
  madler@alumni.caltech.edu
 */

// Convert gzip (.gz) file to a single entry zip file. See the comments before
// gz2zip() for more details and caveats.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(MSDOS) || defined(OS2) || defined(WIN32) || defined(__CYGWIN__)
#  include <fcntl.h>
#  include <io.h>
#  define SET_BINARY_MODE(file) setmode(fileno(file), O_BINARY)
#else
#  define SET_BINARY_MODE(file)
#endif

#define local static

// Exit on error.
local void bail(char *why) {
    fprintf(stderr, "gz2zip abort: %s\n", why);
    exit(1);
}

// Type to track number of bytes written.
typedef struct {
    FILE *out;
    off_t off;
} tally_t;

// Write len bytes at dat to t.
local void put(tally_t *t, void const *dat, size_t len) {
    size_t ret = fwrite(dat, 1, len, t->out);
    if (ret != len)
        bail("write error");
    t->off += len;
}

// Write 16-bit integer n in little-endian order to t.
local void put2(tally_t *t, unsigned n) {
    unsigned char dat[2];
    dat[0] = n;
    dat[1] = n >> 8;
    put(t, dat, 2);
}

// Write 32-bit integer n in little-endian order to t.
local void put4(tally_t *t, unsigned long n) {
    put2(t, n);
    put2(t, n >> 16);
}

// Write n zeros to t.
local void putz(tally_t *t, unsigned n) {
    unsigned char const buf[1] = {0};
    while (n--)
        put(t, buf, 1);
}

// Convert the Unix time unix to DOS time in the four bytes at *dos. If there
// is a conversion error for any reason, store the current time in DOS format
// at *dos. The Unix time in seconds is rounded up to an even number of
// seconds, since the DOS time can only represent even seconds. If the Unix
// time is before 1980, the minimum DOS time of Jan 1, 1980 is used.
local void unix2dos(unsigned char *dos, time_t unixx) {
    unixx += unixx & 1;
    struct tm *s = localtime(&unixx);
    if (s == NULL) {
        unixx = time(NULL);              // on error, use current time
        unixx += unixx & 1;
        s = localtime(&unixx);
        if (s == NULL)
            bail("internal error");     // shouldn't happen
    }
    if (s->tm_year < 80) {              // no DOS time before 1980
        dos[0] = 0;  dos[1] = 0;                // use midnight,
        dos[2] = (1 << 5) + 1;  dos[3] = 0;     // Jan 1, 1980
    }
    else {
        dos[0] = (s->tm_min << 5) + (s->tm_sec >> 1);
        dos[1] = (s->tm_hour << 3) + (s->tm_min >> 3);
        dos[2] = ((s->tm_mon + 1) << 5) + s->tm_mday;
        dos[3] = ((s->tm_year - 80) << 1) + ((s->tm_mon + 1) >> 3);
    }
}

// Chunk size for reading and writing raw deflate data.
#define CHUNK 16384

// Read the gzip file from in and write it as a single-entry zip file to out.
// This assumes that the gzip file has a single member, that it has no junk
// after the gzip trailer, and that it contains less than 4GB of uncompressed
// data. The gzip file is not decompressed or validated, other than checking
// for the proper header format. The modification time from the gzip header is
// used for the zip entry, unless it is not present, in which case the current
// local time is used for the zip entry. The file name from the gzip header is
// used for the zip entry, unless it is not present, in which case "-" is used.
// This does not use the Zip64 format, so the offsets in the resulting zip file
// must be less than 4GB. If name is not NULL, then the zero-terminated string
// at name is used as the file name for the single entry. Whether the file name
// comes from the gzip header or from name, it is truncated to 64K-1 characters
// if necessary.
//
// It is recommended that unzip -t be used on the resulting file to verify its
// integrity. If the gzip files do not obey the constraints above, then the zip
// file will not be valid.
local void gz2zip(FILE *in, FILE *out, char *name) {
    // zip file constant headers for local, central, and end record
    unsigned char const loc[] = {'P', 'K', 3, 4, 20, 0, 8, 0, 8, 0};
    unsigned char const cen[] = {'P', 'K', 1, 2, 20, 0, 20, 0, 8, 0, 8, 0};
    unsigned char const end[] = {'P', 'K', 5, 6, 0, 0, 0, 0, 1, 0, 1, 0};

    // gzip header
    unsigned char head[10];

    // zip file modification date, CRC, and sizes -- initialize to zero for the
    // local header (the actual CRC and sizes follow the compressed data)
    unsigned char desc[16] = {0};

    // name from gzip header to use for the zip entry (the maximum size of the
    // name is 64K-1 -- if the gzip name is longer, then it is truncated)
    unsigned name_len;
    char save[65535];

    // read and interpret the gzip header, bailing if it is invalid or has an
    // unknown compression method or flag bits set
    size_t got = fread(head, 1, sizeof(head), in);
    if (got < sizeof(head) ||
        head[0] != 0x1f || head[1] != 0x8b || head[2] != 8 || (head[3] & 0xe0))
        bail("input not gzip");
    if (head[3] & 4) {                  // extra field (ignore)
        unsigned extra = getc(in);
        int high = getc(in);
        if (high == EOF)
            bail("premature end of gzip input");
        extra += (unsigned)high << 8;
        fread(name, 1, extra, in);
    }
    if (head[3] & 8) {                  // file name (save)
        name_len = 0;
        int ch;
        while ((ch = getc(in)) != 0 && ch != EOF)
            if (name_len < sizeof(name))
                save[name_len++] = ch;
    }
    else {                              // no file name
        name_len = 1;
        save[0] = '-';
    }
    if (head[3] & 16) {                 // comment (ignore)
        int ch;
        while ((ch = getc(in)) != 0 && ch != EOF)
            ;
    }
    if (head[3] & 2) {                  // header crc (ignore)
        getc(in);
        getc(in);
    }

    // use name from argument if present, otherwise from gzip header
    if (name == NULL)
        name = save;
    else {
        name_len = strlen(name);
        if (name_len > 65535)
            name_len = 65535;
    }

    // set modification time and date in descriptor from gzip header
    time_t mod = head[4] + (head[5] << 8) + ((time_t)(head[6]) << 16) +
                 ((time_t)(head[7]) << 24);
    unix2dos(desc, mod ? mod : time(NULL));

    // initialize tally of output bytes
    tally_t zip = {out, 0};

    // write zip local header
    off_t locoff = zip.off;
    put(&zip, loc, sizeof(loc));
    put(&zip, desc, sizeof(desc));
    put2(&zip, name_len);
    putz(&zip, 2);
    put(&zip, name, name_len);

    // copy raw deflate stream, saving eight-byte gzip trailer
    unsigned char buf[CHUNK + 8];
    if (fread(buf, 1, 8, in) != 8)
        bail("premature end of gzip input");
    off_t comp = 0;
    while ((got = fread(buf + 8, 1, CHUNK, in)) != 0) {
        put(&zip, buf, got);
        comp += got;
        memmove(buf, buf + got, 8);
    }

    // write descriptor based on gzip trailer and compressed count
    memcpy(desc + 4, buf, 4);
    desc[8] = comp;
    desc[9] = comp >> 8;
    desc[10] = comp >> 16;
    desc[11] = comp >> 24;
    memcpy(desc + 12, buf + 4, 4);
    put(&zip, desc + 4, sizeof(desc) - 4);

    // write zip central directory
    off_t cenoff = zip.off;
    put(&zip, cen, sizeof(cen));
    put(&zip, desc, sizeof(desc));
    put2(&zip, name_len);
    putz(&zip, 12);
    put4(&zip, locoff);
    put(&zip, name, name_len);

    // write zip end-of-central-directory record
    off_t endoff = zip.off;
    put(&zip, end, sizeof(end));
    put4(&zip, endoff - cenoff);
    put4(&zip, cenoff);
    putz(&zip, 2);
}

// Convert the gzip file on stdin to a zip file on stdout. If present, the
// first argument is used as the file name in the zip entry.
int main(int argc, char **argv) {
    // avoid end-of-line conversions on evil operating systems
    SET_BINARY_MODE(stdin);
    SET_BINARY_MODE(stdout);

    // convert .gz on stdin to .zip on stdout -- error returns use exit()
    gz2zip(stdin, stdout, argc > 1 ? argv[1] : NULL);
    return 0;
}
