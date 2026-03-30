SRCS := rwd_main.c rwd_dtls.c rwd_ice.c rwd_sdp.c rwd_signaling.c rwd_media.c
ifeq ($(WEBTORRENT),1)
SRCS += rwd_webtorrent.c
endif
OBJS := $(SRCS:.c=.o)
BIN  := rwd

all: $(BIN)

$(BIN): $(OBJS)
	@echo "  LD      $@"
	$(Q)$(CC) -o $@ $^ $(LIBS) $(LDFLAGS)

%.o: %.c
	@echo "  CC      $<"
	$(Q)$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(BIN)

-include $(OBJS:.o=.d)

.PHONY: all clean
