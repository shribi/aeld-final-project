# Native build:   make
# Cross build:    make CROSS_COMPILE=aarch64-linux-gnu-
# Parser tests:   make test

CROSS_COMPILE ?=
CC      := $(CROSS_COMPILE)gcc
CFLAGS  ?= -std=gnu17 -Wall -Wextra -O2
LDFLAGS ?= -static

TARGET := inmpTest
SRC    := inmpTest.c

PARSER_DIR  := ../../inmp-module
PARSER_TEST := parser_test

all: $(TARGET)

$(TARGET): $(SRC) $(PARSER_DIR)/inmp_ioctl.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

$(PARSER_TEST): parser_test.c $(PARSER_DIR)/inmp_parser.c $(PARSER_DIR)/inmp_parser.h
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ parser_test.c $(PARSER_DIR)/inmp_parser.c

test: $(PARSER_TEST)
	./$(PARSER_TEST)

clean:
	rm -f $(TARGET) $(PARSER_TEST) *.o

.PHONY: all clean test
