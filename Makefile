CC ?= cc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra

TURTLE_SRC = \
	third_party/turtledeflate/lib/turtledeflate.c \
	third_party/turtledeflate/lib/turtledeflate_tree.c \
	third_party/turtledeflate/lib/turtledeflate_block.c \
	third_party/turtledeflate/lib/turtledeflate_bitstream.c

turzip: turzip.c $(TURTLE_SRC)
	$(CC) $(CFLAGS) -pthread -Ithird_party/turtledeflate/inc -Ithird_party/turtledeflate/lib -o $@ turzip.c $(TURTLE_SRC) -lm

clean:
	rm -f turzip

.PHONY: clean
