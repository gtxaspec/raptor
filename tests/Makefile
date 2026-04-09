CC ?= gcc
CFLAGS = -Wall -Wextra -std=c11 -D_GNU_SOURCE -g -fsanitize=address -I../rmr -I.
LDFLAGS = -fsanitize=address

TEST_SRCS = main.c test_nal.c test_prebuf.c test_mux.c
LIB_SRCS = ../rmr/rmr_nal.c ../rmr/rmr_prebuf.c ../rmr/rmr_mux.c

tests: $(TEST_SRCS) $(LIB_SRCS) greatest.h
	$(CC) $(CFLAGS) -o $@ $(TEST_SRCS) $(LIB_SRCS) $(LDFLAGS)

test: tests
	./tests

clean:
	rm -f tests

.PHONY: test clean
