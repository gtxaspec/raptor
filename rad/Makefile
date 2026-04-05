SRCS := rad_main.c rad_codec.c rad_codec_g711.c rad_codec_l16.c rad_codec_aac.c rad_codec_opus.c
OBJS := $(SRCS:.c=.o)
BIN  := rad

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
