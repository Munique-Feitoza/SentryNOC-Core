CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion
LDFLAGS ?=

TARGET := sentrynoc-agent
SRC := src/sentrynoc.c

.PHONY: all clean test run-collector replay

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDFLAGS)

test: $(TARGET)
	./$(TARGET) --once --interval-ms 100 --spool /tmp/sentrynoc-test.ndjson

run-collector:
	python3 collector/server.py

replay:
	python3 collector/replay_spool.py $(SPOOL)

clean:
	rm -f $(TARGET)
