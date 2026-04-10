CC     := clang
# compy's cmake deps (interface99, slice99, etc.) live in build/_deps/
COMPY_DEPS := $(wildcard ../../compy/build/_deps/*-src) \
              $(wildcard ../../compy/build/_deps/*-src/include)
CFLAGS := -g -fsanitize=fuzzer,address -I../rwd -I../../raptor-common/include \
          -I../../raptor-ipc/include -I../../compy/include \
          $(addprefix -I,$(COMPY_DEPS)) -I.
LDFLAGS := -fsanitize=fuzzer,address -lm

# raptor-common sources needed by SDP/auth harnesses
COMMON_SRCS := ../../raptor-common/src/rss_util.c \
               ../../raptor-common/src/rss_log.c \
               ../../raptor-common/src/rss_http.c \
               ../../raptor-common/src/cJSON.c

TARGETS := fuzz_sdp fuzz_stun fuzz_http_auth

all: $(TARGETS)

# SDP parser: extract parse function from rwd_sdp.c, compile with shim.
# sed strips everything after rwd_sdp_generate_answer (the answer generator
# needs full rwd_client_t/rwd_server_t which we don't want to pull in).
fuzz_sdp: fuzz_sdp_parse.c $(COMMON_SRCS)
	sed '/^int rwd_sdp_generate_answer/,$$d' ../rwd/rwd_sdp.c > /tmp/fuzz_sdp_src.c
	$(CC) $(CFLAGS) -DRWD_H -include fuzz_rwd_shim.h -o $@ $< /tmp/fuzz_sdp_src.c $(COMMON_SRCS) $(LDFLAGS)

# STUN parser: self-contained (parsing logic duplicated in harness)
fuzz_stun: fuzz_stun_parse.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# HTTP auth: needs raptor-common (base64, auth check)
fuzz_http_auth: fuzz_http_auth.c $(COMMON_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean
