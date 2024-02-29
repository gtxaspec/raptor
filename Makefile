CC = mipsel-linux-gcc
CFLAGS = -Wall -g

SOURCES = rv4.c
OBJECTS = $(SOURCES:.c=.o)
EXECUTABLE = rv4

all: $(EXECUTABLE)

$(EXECUTABLE): $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f $(OBJECTS) $(EXECUTABLE)
