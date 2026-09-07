CC = gcc
CFLAGS = -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE -Ic_src -Ithird_party/zopfli/src -Ithird_party/bzip2
LDFLAGS = -lm -lz -llzma -lzstd
HAVE_DEFLATE = $(shell test -f /usr/include/libdeflate.h && echo 1 || echo 0)
ifeq ($(HAVE_DEFLATE),1)
CFLAGS += -DHAVE_LIBDEFLATE
LDFLAGS += -ldeflate
endif
CFLAGS += -DHAVE_LZMA -DHAVE_ZSTD

ZOPFLI_SRCS = third_party/zopfli/src/zopfli/blocksplitter.c \
  third_party/zopfli/src/zopfli/cache.c \
  third_party/zopfli/src/zopfli/deflate.c \
  third_party/zopfli/src/zopfli/hash.c \
  third_party/zopfli/src/zopfli/katajainen.c \
  third_party/zopfli/src/zopfli/lz77.c \
  third_party/zopfli/src/zopfli/squeeze.c \
  third_party/zopfli/src/zopfli/tree.c \
  third_party/zopfli/src/zopfli/util.c \
  third_party/zopfli/src/zopfli/zlib_container.c \
  third_party/zopfli/src/zopfli/gzip_container.c \
  third_party/zopfli/src/zopfli/zopfli_lib.c
BZIP2_SRCS = third_party/bzip2/blocksort.c \
  third_party/bzip2/huffman.c \
  third_party/bzip2/crctable.c \
  third_party/bzip2/randtable.c \
  third_party/bzip2/compress.c \
  third_party/bzip2/decompress.c \
  third_party/bzip2/bzlib.c
SRCS = c_src/policy.c c_src/competitor.c c_src/archiver.c c_src/main.c $(ZOPFLI_SRCS) $(BZIP2_SRCS)
OBJS = $(SRCS:.c=.o)
TARGET = katzip
STATIC_TARGET = katzip_static

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET) $(STATIC_TARGET) maxzip_c maxzip_static

test: $(TARGET)
	./tests_c.sh

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

.PHONY: all clean test install static

static: $(OBJS)
	$(CC) $(OBJS) -o $(STATIC_TARGET) -static $(LDFLAGS) -lm
