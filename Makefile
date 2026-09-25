CC ?= cc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra
CMAKE ?= cmake

TURTLE_SRC = \
	third_party/turtledeflate/lib/turtledeflate.c \
	third_party/turtledeflate/lib/turtledeflate_tree.c \
	third_party/turtledeflate/lib/turtledeflate_block.c \
	third_party/turtledeflate/lib/turtledeflate_bitstream.c
TURTLE_HEADERS = $(wildcard third_party/turtledeflate/inc/*.h third_party/turtledeflate/lib/*.h)
MINIZIP_FILES = $(wildcard third_party/minizip-ng/*.[ch] third_party/minizip-ng/CMakeLists.txt)

turzip: turzip.c $(TURTLE_SRC) $(TURTLE_HEADERS) $(MINIZIP_FILES) patches/turtledeflate.patch Makefile
	@set -eu; \
	build_dir=$$(mktemp -d); \
	trap 'rm -rf "$$build_dir"' EXIT; \
	cp -R third_party/turtledeflate/inc third_party/turtledeflate/lib "$$build_dir"/; \
	git -C "$$build_dir" apply "$(CURDIR)/patches/turtledeflate.patch"; \
	$(CMAKE) -S third_party/minizip-ng -B "$$build_dir/minizip" \
	  -DMZ_ZLIB=ON -DMZ_ZLIB_FLAVOR=zlib -DMZ_BZIP2=OFF -DMZ_LZMA=OFF \
	  -DMZ_ZSTD=OFF -DMZ_PPMD=OFF -DMZ_PKCRYPT=OFF -DMZ_WZAES=OFF \
	  -DMZ_OPENSSL=OFF -DMZ_FETCH_LIBS=OFF -DMZ_BUILD_TESTS=OFF \
	  -DMZ_BUILD_UNIT_TESTS=OFF -DMZ_COMPAT=OFF >/dev/null; \
	$(CMAKE) --build "$$build_dir/minizip" --parallel 2 >/dev/null; \
	$(CC) $(CFLAGS) -pthread -I"$$build_dir/inc" -I"$$build_dir/lib" \
	  -Ithird_party/minizip-ng -I"$$build_dir/minizip" -o $@ turzip.c \
	  "$$build_dir/lib/turtledeflate.c" "$$build_dir/lib/turtledeflate_tree.c" \
	  "$$build_dir/lib/turtledeflate_block.c" "$$build_dir/lib/turtledeflate_bitstream.c" \
	  "$$build_dir/minizip/libminizip-ng.a" -lz -lm

clean:
	rm -f turzip

test: turzip
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -v

.PHONY: clean test
