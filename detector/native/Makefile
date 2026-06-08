# Native (host) regression build for the C detector port.
#
# Compiles the SAME pure-C cores the ESP32 firmware uses (background.c,
# tracker.c) plus the test_replay.c driver into a host executable. No ESP-IDF,
# no PlatformIO -- if it builds with bare gcc, the detector headers are clean of
# hardware deps by construction.
#
# Usage:
#   make            # build ./replay(.exe)
#   make regression # build + run the python tolerance check over all fixtures
#   make clean
#
# Windows note: GNU Make here is invoked as `mingw32-make`. gcc is MinGW-W64.

CC      ?= gcc
DET      = ../../doorway-node-01/src/detection
CFLAGS   = -O2 -Wall -Wextra -std=c11 -I$(DET)
LDLIBS   = -lm
SRCS     = $(DET)/background.c $(DET)/tracker.c test_replay.c
HDRS     = $(DET)/background.h $(DET)/tracker.h

# Executable name: .exe on Windows, bare elsewhere.
ifeq ($(OS),Windows_NT)
TARGET = replay.exe
RM     = del /Q
else
TARGET = replay
RM     = rm -f
endif

$(TARGET): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LDLIBS)

.PHONY: regression clean
regression: $(TARGET)
	python run_regression.py

clean:
	-$(RM) $(TARGET)
