CC ?= gcc
CFLAGS = -Wall -Wextra -std=c11 -D_GNU_SOURCE -g -fsanitize=address \
         -I../rmr -I../rwd -I../../raptor-common/include -I.
LDFLAGS = -fsanitize=address

TEST_SRCS = main.c test_nal.c test_prebuf.c test_mux.c test_codec.c test_sdp.c
LIB_SRCS = ../rmr/rmr_nal.c ../rmr/rmr_prebuf.c ../rmr/rmr_mux.c \
           ../../raptor-common/src/rss_util.c ../../raptor-common/src/rss_log.c

# rwd_sdp.c includes rwd.h which pulls in compy+mbedTLS — compile it with
# a minimal shim header instead (only needs rwd_sdp_offer_t + rss_strlcpy).
SDP_SRC = sdp_parse.c
$(SDP_SRC): ../rwd/rwd_sdp.c
	sed '/^int rwd_sdp_generate_answer/,$$d' $< > $@

tests: $(TEST_SRCS) $(LIB_SRCS) $(SDP_SRC) greatest.h
	$(CC) $(CFLAGS) -DRWD_H -include ../fuzz/fuzz_rwd_shim.h \
		-o $@ $(TEST_SRCS) $(LIB_SRCS) $(SDP_SRC) $(LDFLAGS)

test: tests
	./tests

clean:
	rm -f tests

.PHONY: test clean
