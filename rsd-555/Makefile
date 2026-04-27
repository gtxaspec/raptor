CXX    ?= $(CROSS_COMPILE)g++
CC     ?= $(CROSS_COMPILE)gcc

SRCS_C   := rsd555_queue.c rsd555_reader.c rsd555_ctrl.c
SRCS_CXX := rsd555_main.cpp rsd555_source.cpp rsd555_subsession.cpp

OBJS := $(SRCS_C:.c=.o) $(SRCS_CXX:.cpp=.o)
BIN  := rsd-555

# live555 include paths (set by parent Makefile or standalone build)
LIVE555_INC ?=
LIVE555_LIBS ?=

CXXFLAGS_LOCAL := $(filter-out -std=gnu11,$(CFLAGS)) -std=c++20 \
	-DNO_OPENSSL -DNO_STD_LIB \
	$(LIVE555_INC)
CFLAGS_LOCAL   := $(CFLAGS)

all: $(BIN)

$(BIN): $(OBJS)
	@echo "  LD      $@"
	$(Q)$(CXX) -o $@ $^ $(LIBS) $(LIVE555_LIBS) $(LDFLAGS) -lstdc++

%.o: %.cpp
	@echo "  CXX     $<"
	$(Q)$(CXX) $(CXXFLAGS_LOCAL) -MMD -MP -c $< -o $@

%.o: %.c
	@echo "  CC      $<"
	$(Q)$(CC) $(CFLAGS_LOCAL) -MMD -MP -c $< -o $@

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(BIN)

-include $(OBJS:.o=.d)

.PHONY: all clean
