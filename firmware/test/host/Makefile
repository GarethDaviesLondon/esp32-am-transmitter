# Host build of the DSP layer. No ESP-IDF, no hardware.
#
#   make test     build and run
#   make clean
#
# main/audio/dsp.c is written to compile here as well as on the target; if a
# change to it breaks this build, that is the change's problem, not the test's.

CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion
CFLAGS  += -I../../main/audio
LDLIBS  += -lm

SRC = test_dsp.c ../../main/audio/dsp.c
BIN = test_dsp

.PHONY: test clean
test: $(BIN)
	./$(BIN)

$(BIN): $(SRC) ../../main/audio/dsp.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

clean:
	rm -f $(BIN)
