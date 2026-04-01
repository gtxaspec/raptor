BIN  := rlatency
SRCS := rlatency.c
OBJS := $(SRCS:.c=.o)

all: $(BIN)

$(BIN): $(OBJS)
	@echo "  LD      $@"
	$(Q)$(CC) -o $@ $^ $(LDFLAGS) -lm

%.o: %.c
	@echo "  CC      $<"
	$(Q)$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(BIN)

-include $(OBJS:.o=.d)

.PHONY: all clean
