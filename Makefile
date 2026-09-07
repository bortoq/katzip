CC = gcc
CFLAGS = -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE -Isrc -Ithird_party/zopfli/src
LDFLAGS = -lm -lz
HAVE_DEFLATE = $(shell test -f /usr/include/libdeflate.h && echo 1 || echo 0)
ifeq ($(HAVE_DEFLATE),1)
CFLAGS += -DHAVE_LIBDEFLATE
LDFLAGS += -ldeflate
endif

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

SRCS = src/policy.c src/competitor.c src/archiver.c src/main.c $(ZOPFLI_SRCS)
OBJS = $(SRCS:.c=.o)
TARGET = katzip
STATIC_TARGET = katzip_static

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

static: $(OBJS)
	$(CC) $(OBJS) -o $(STATIC_TARGET) -static $(LDFLAGS) -lm

clean:
	rm -f $(OBJS) $(TARGET) $(STATIC_TARGET)

test: $(TARGET)
	./tests_c.sh

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/

.PHONY: all clean test install static
