#include "policy.h"
#include <string.h>
#include <strings.h>
#include <math.h>
#include <ctype.h>

/* NOTE: .pdf removed — text PDFs compress well. Only truly
 * already-compressed media/archives are skipped. */
static const char *incompressible_exts[] = {
    ".jpg",".jpeg",".png",".gif",".webp",".avif",
    ".mp4",".mkv",".avi",".mov",".mp3",".ogg",".flac",
    ".zip",".gz",".bz2",".xz",".7z",".zst",".rar",
    ".woff",".woff2",
    ".mpg",".mpeg",".webm",".opus",
    NULL
};

bool is_incompressible_by_ext(const char *filename) {
    if (!filename || *filename=='\0') return false;
    const char *dot = strrchr(filename, '.');
    if (!dot) return false;
    for (int i=0; incompressible_exts[i]; i++) {
        if (strcasecmp(dot, incompressible_exts[i])==0) return true;
    }
    return false;
}

double calc_entropy(const unsigned char *data, size_t len) {
    if (!data || len==0) return 0.0;
    /* sample first 32KB for speed on big files */
    size_t n = len > 32768 ? 32768 : len;
    size_t freq[256]={0};
    for (size_t i=0;i<n;i++) freq[data[i]]++;
    double ent=0.0;
    for (int i=0;i<256;i++) if (freq[i]) {
        double p=(double)freq[i]/(double)n;
        ent -= p * (log(p)/log(2.0));
    }
    return ent;
}

bool is_high_entropy(const unsigned char *data, size_t len) {
    if (!data || len < 256) return false;
    double e = calc_entropy(data,len);
    return e > ENTROPY_THRESHOLD;
}

bool should_use_store_only(const char *filename, const unsigned char *data, size_t len) {
    if (!data) return true;
    if (len==0) return true;
    if (is_incompressible_by_ext(filename)) return true;
    if (is_high_entropy(data,len)) return true;
    return false;
}

bool should_skip_heavy(const char *filename, const unsigned char *data, size_t len, int level) {
    (void)filename; (void)data;
    if (level < 3) return true;
    if (!data || len < SMALL_FILE_THRESHOLD) return true;
    if (len > ZOPFLI_SIZE_LIMIT && level >=4) return true;
    return false;
}

bool zopfli_allowed(const unsigned char *data, size_t len, int level) {
    if (level < 4) return false;
    if (!data) return false;
    if (len < SMALL_FILE_THRESHOLD) return false;
    if (len > ZOPFLI_SIZE_LIMIT) return false;
    return true;
}
