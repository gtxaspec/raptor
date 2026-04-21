SRCS := rod_main.c rod_elem.c rod_config.c rod_template.c rod_ctrl.c \
       rod_render.c rod_receipt.c
OBJS := $(SRCS:.c=.o)
BIN  := rod

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
