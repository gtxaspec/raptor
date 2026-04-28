CC ?= gcc

# Sanitizer selection — `address` (default) or `thread`. ASAN and TSAN
# are mutually exclusive; CI runs both in separate jobs.
SAN ?= address

CFLAGS = -Wall -Wextra -std=c11 -D_GNU_SOURCE -g -fsanitize=$(SAN) \
         -I../rmr -I../rwd -I../rsp -I../rvd -I../../raptor-hal/include \
         -I../../raptor-common/include -I../../raptor-ipc/include -I.
LDFLAGS = -fsanitize=$(SAN) -lpthread -lrt -lm

TEST_SRCS = main.c test_nal.c test_prebuf.c test_mux.c test_codec.c test_sdp.c test_ring.c test_rsp.c \
            test_ctrl.c
LIB_SRCS = ../rmr/rmr_nal.c ../rmr/rmr_prebuf.c ../rmr/rmr_mux.c \
           ../../raptor-common/src/rss_util.c ../../raptor-common/src/rss_log.c \
           ../../raptor-common/src/cJSON.c
CTRL_SRCS = ../rvd/rvd_ctrl.c mock_hal.c \
            ../../raptor-common/src/rss_config.c ../../raptor-common/src/rss_ctrl_cmds.c
IPC_SRCS = ../../raptor-ipc/src/rss_ring.c ../../raptor-ipc/src/rss_ctrl.c \
           ../../raptor-ipc/src/rss_osd_shm.c ../../raptor-ipc/src/rss_ipc_log.c

# rwd_sdp.c includes rwd.h which pulls in compy+mbedTLS — compile it with
# a minimal shim header instead (only needs rwd_sdp_offer_t + rss_strlcpy).
SDP_SRC = sdp_parse.c
$(SDP_SRC): ../rwd/rwd_sdp.c
	sed '/^int rwd_sdp_generate_answer/,$$d' $< > $@

tests: $(TEST_SRCS) $(LIB_SRCS) $(CTRL_SRCS) $(IPC_SRCS) $(SDP_SRC) greatest.h
	$(CC) $(CFLAGS) -DRWD_H -include ../fuzz/fuzz_rwd_shim.h \
		-o $@ $(TEST_SRCS) $(LIB_SRCS) $(CTRL_SRCS) $(IPC_SRCS) $(SDP_SRC) $(LDFLAGS)

test: tests
	./tests

clean:
	rm -f tests sdp_parse.c

.PHONY: test clean
