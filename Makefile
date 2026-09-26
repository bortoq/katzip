CC ?= cc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra
ECT_CFLAGS ?= -O2 -std=gnu99
CXX ?= c++
CXXFLAGS ?= -O2 -std=c++11
CMAKE ?= cmake

TURTLE_SRC = \
	third_party/turtledeflate/lib/turtledeflate.c \
	third_party/turtledeflate/lib/turtledeflate_tree.c \
	third_party/turtledeflate/lib/turtledeflate_block.c \
	third_party/turtledeflate/lib/turtledeflate_bitstream.c
TURTLE_HEADERS = $(wildcard third_party/turtledeflate/inc/*.h third_party/turtledeflate/lib/*.h)
MINIZIP_FILES = $(wildcard third_party/minizip-ng/*.[ch] third_party/minizip-ng/CMakeLists.txt)
ECT_FILES = $(wildcard third_party/ect/src/*.[ch] third_party/ect/src/zopfli/*.[ch] third_party/ect/src/zopfli/*.cpp)

KATZIP_SOURCES = $(wildcard src/*.c)
KATZIP_HEADERS = $(wildcard src/*.h)
SANITIZER_FLAGS ?=

katzip-asan: SANITIZER_FLAGS = -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
katzip-tsan: SANITIZER_FLAGS = -O1 -g -fsanitize=thread -fno-omit-frame-pointer

katzip katzip-asan katzip-tsan: $(KATZIP_SOURCES) $(KATZIP_HEADERS) $(TURTLE_SRC) $(TURTLE_HEADERS) $(MINIZIP_FILES) $(ECT_FILES) patches/turtledeflate.patch patches/turtledeflate_bounds.patch patches/ect.patch Makefile
	@set -eu; \
	build_dir=$$(mktemp -d); \
	trap 'rm -rf "$$build_dir"' EXIT; \
	cp -R third_party/turtledeflate/inc third_party/turtledeflate/lib "$$build_dir"/; \
	cp -R third_party/ect/src "$$build_dir/ect"; \
	git -C "$$build_dir/ect" apply "$(CURDIR)/patches/ect.patch"; \
	git -C "$$build_dir" apply "$(CURDIR)/patches/turtledeflate.patch"; \
	tr -d '\r' < "$$build_dir/lib/turtledeflate_block.c" \
	  > "$$build_dir/turtledeflate_block.normalized.c"; \
	mv "$$build_dir/turtledeflate_block.normalized.c" \
	  "$$build_dir/lib/turtledeflate_block.c"; \
	git -C "$$build_dir" apply "$(CURDIR)/patches/turtledeflate_bounds.patch"; \
	$(CMAKE) -S third_party/minizip-ng -B "$$build_dir/minizip" \
	  -DMZ_ZLIB=ON -DMZ_ZLIB_FLAVOR=zlib -DMZ_BZIP2=OFF -DMZ_LZMA=OFF \
	  -DMZ_ZSTD=OFF -DMZ_PPMD=OFF -DMZ_PKCRYPT=OFF -DMZ_WZAES=OFF \
	  -DMZ_OPENSSL=OFF -DMZ_FETCH_LIBS=OFF -DMZ_BUILD_TESTS=OFF \
	  -DMZ_BUILD_UNIT_TESTS=OFF -DMZ_COMPAT=OFF \
	  -DCMAKE_C_FLAGS="$(SANITIZER_FLAGS)" \
	  -DCMAKE_CXX_FLAGS="$(SANITIZER_FLAGS)" >/dev/null; \
	$(CMAKE) --build "$$build_dir/minizip" --parallel 2 >/dev/null; \
	for source in blocksplitter lz77 squeeze util; do \
	  $(CC) $(ECT_CFLAGS) $(SANITIZER_FLAGS) -c "$$build_dir/ect/zopfli/$$source.c" -o "$$build_dir/ect_$$source.o"; \
	done; \
	$(CC) $(ECT_CFLAGS) $(SANITIZER_FLAGS) -c "$$build_dir/ect/LzFind.c" -o "$$build_dir/ect_LzFind.o"; \
	for source in deflate katajainen; do \
	  $(CXX) $(CXXFLAGS) $(SANITIZER_FLAGS) -c "$$build_dir/ect/zopfli/$$source.cpp" -o "$$build_dir/ect_$$source.o"; \
	done; \
	for source in turtledeflate turtledeflate_tree turtledeflate_block turtledeflate_bitstream; do \
	  $(CC) $(CFLAGS) $(SANITIZER_FLAGS) -I"$$build_dir/inc" -I"$$build_dir/lib" \
	    -c "$$build_dir/lib/$$source.c" -o "$$build_dir/$$source.o"; \
	done; \
	for source in $(KATZIP_SOURCES); do \
	  object=$${source##*/}; object=$${object%.c}; \
	  $(CC) $(CFLAGS) $(SANITIZER_FLAGS) -pthread -I$(CURDIR) -I"$$build_dir/inc" -I"$$build_dir/lib" \
	    -Ithird_party/minizip-ng -I"$$build_dir/minizip" \
	    -c "$$source" -o "$$build_dir/katzip_$$object.o"; \
	done; \
	$(CXX) $(SANITIZER_FLAGS) -pthread -o $@ "$$build_dir"/katzip_*.o \
	  "$$build_dir/turtledeflate.o" "$$build_dir/turtledeflate_tree.o" \
	  "$$build_dir/turtledeflate_block.o" "$$build_dir/turtledeflate_bitstream.o" \
	  "$$build_dir"/ect_*.o "$$build_dir/minizip/libminizip-ng.a" -ldeflate -lz -lm

asan: katzip-asan
	KATZIP_PROGRAM="$(CURDIR)/katzip-asan" KATZIP_TEST_TIMEOUT_SCALE=6 \
	  PYTHONDONTWRITEBYTECODE=1 \
	  python3 -m unittest discover -s tests -v

tsan: katzip-tsan
	KATZIP_PROGRAM="$(CURDIR)/katzip-tsan" KATZIP_TEST_TIMEOUT_SCALE=6 \
	  PYTHONDONTWRITEBYTECODE=1 \
	  python3 -m unittest discover -s tests -v

clean:
	rm -f katzip katzip-asan katzip-tsan

# Runtime-generated INI files may contain user edits and are preserved.
distclean: clean

test: katzip
	PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -v

.PHONY: clean distclean test asan tsan
