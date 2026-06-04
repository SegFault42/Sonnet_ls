CC      = cc
CFLAGS  = -Wall -Wextra -std=c11 -O2 \
          -isysroot $(shell xcrun --show-sdk-path) \
          -D_DARWIN_C_SOURCE
LDFLAGS = -lutil

SRCS = main.c options.c sort.c print.c output.c color.c
OBJS = $(SRCS:.c=.o)
TARGET = ls

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c ls.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)
