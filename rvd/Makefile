SRCS := rvd_main.c rvd_pipeline.c rvd_frame_loop.c rvd_ctrl.c rvd_osd.c rvd_osd_pos.c rvd_osd_pool.c rvd_ivs.c rvd_ring_size.c rvd_frame_result.c rvd_ivs_raw.c
OBJS := $(SRCS:.c=.o)
BIN  := rvd

all: $(BIN)

$(BIN): $(OBJS) $(LIB_DEPS)
	@echo "  LD      $@"
	$(Q)$(CC) -o $@ $(OBJS) $(LIBS) $(LDFLAGS)

%.o: %.c
	@echo "  CC      $<"
	$(Q)$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(BIN)

-include $(OBJS:.o=.d)

.PHONY: all clean
