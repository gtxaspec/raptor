SRCS := rsp_main.c rsp_rtmp.c rsp_audio.c
OBJS := $(SRCS:.c=.o) rmr_nal.o
BIN  := rsp

RMR_DIR ?= ../rmr
CFLAGS += -I$(RMR_DIR)

all: $(BIN)

$(BIN): $(OBJS)
	@echo "  LD      $@"
	$(Q)$(CC) -o $@ $^ $(LIBS) $(LDFLAGS)

rmr_nal.o: $(RMR_DIR)/rmr_nal.c
	@echo "  CC      rmr_nal.c"
	$(Q)$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

%.o: %.c
	@echo "  CC      $<"
	$(Q)$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(BIN)

-include $(OBJS:.o=.d)

.PHONY: all clean
