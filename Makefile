CC ?= cc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra

TURTLE_SRC = \
	third_party/turtledeflate/lib/turtledeflate.c \
	third_party/turtledeflate/lib/turtledeflate_tree.c \
	third_party/turtledeflate/lib/turtledeflate_block.c \
	third_party/turtledeflate/lib/turtledeflate_bitstream.c
TURTLE_HEADERS = $(wildcard third_party/turtledeflate/inc/*.h third_party/turtledeflate/lib/*.h)

turzip: turzip.c $(TURTLE_SRC) $(TURTLE_HEADERS) patches/turtledeflate.patch Makefile
	@set -eu; \
	build_dir=$$(mktemp -d); \
	trap 'rm -rf "$$build_dir"' EXIT; \
	cp -R third_party/turtledeflate/inc third_party/turtledeflate/lib "$$build_dir"/; \
	git -C "$$build_dir" apply "$(CURDIR)/patches/turtledeflate.patch"; \
	$(CC) $(CFLAGS) -pthread -I"$$build_dir/inc" -I"$$build_dir/lib" -o $@ turzip.c \
	  "$$build_dir/lib/turtledeflate.c" "$$build_dir/lib/turtledeflate_tree.c" \
	  "$$build_dir/lib/turtledeflate_block.c" "$$build_dir/lib/turtledeflate_bitstream.c" -lm

clean:
	rm -f turzip

.PHONY: clean
