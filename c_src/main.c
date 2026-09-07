#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "archiver.h"

static void print_usage(void){
    fprintf(stderr,
        "Usage: katzip <archive.zip> <files...>\n"
        "Example: katzip archive file.txt\n");
}

static char *ensure_zip_extension(const char *archive){
    if (!archive) return NULL;
    size_t len = strlen(archive);
    if (len >= 4 && strcasecmp(archive + len - 4, ".zip") == 0) {
        return strdup(archive);
    }
    // append .zip
    char *out = (char*)malloc(len + 5);
    if (!out) return NULL;
    memcpy(out, archive, len);
    memcpy(out + len, ".zip", 5);
    return out;
}

int main(int argc, char *argv[]){
    if (argc < 2) {
        print_usage();
        return 1;
    }
    // handle help
    if (strcmp(argv[1], "--help")==0 || strcmp(argv[1], "-h")==0 || strcmp(argv[1], "-?")==0) {
        print_usage();
        return 0;
    }
    if (argc < 3) {
        print_usage();
        return 1;
    }
    // reject any option-like arguments (no extra modes)
    for (int i=1;i<argc;i++) {
        if (argv[i][0]=='-' && i!=1) { // archive may start with -? treat as error
            // Actually archive is argv[1], files are argv[2..]
            // So check files for dash prefix - but we disallow extra args
            // Keep simple: if any arg after archive starts with '-', show usage
            // Except we already handled --help
            if (i>=2 && argv[i][0]=='-') {
                fprintf(stderr, "unknown option %s\n", argv[i]);
                print_usage();
                return 1;
            }
        }
    }

    const char *raw_archive = argv[1];
    char *archive = ensure_zip_extension(raw_archive);
    if (!archive) {
        fprintf(stderr, "memory error\n");
        return 1;
    }

    char **files = &argv[2];
    size_t nfiles = (size_t)(argc - 2);

    // most dense mode: level 4, max compat (as per roadmap -4)
    int level = 4;
    int compat = ZIP_COMPAT_MAX;

    bool ok = create_zip(archive, files, nfiles, level, compat);
    if (!ok) {
        fprintf(stderr, "error: failed to create %s\n", archive);
        free(archive);
        return 1;
    }
    free(archive);
    return 0;
}
