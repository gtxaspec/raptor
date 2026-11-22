SRCS := rcd_main.c rcd_tsmap.c rcd_drv_ethcam.c rcd_ethcam.c
OBJS := $(SRCS:.c=.o)
BIN  := rcd

all: $(BIN)

$(BIN): $(OBJS)
	@echo "  LD      $@"
	$(Q)$(CC) -o $@ $^ $(LIBS) -lpthread $(LDFLAGS)

%.o: %.c
	@echo "  CC      $<"
	$(Q)$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(BIN)

-include $(OBJS:.o=.d)

.PHONY: all clean
