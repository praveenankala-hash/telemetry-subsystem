CC := gcc
CFLAGS := -Wall -Wextra -pthread -I../include
BIN_DIR := ../bin

.PHONY: all clean

all: telemetry_user
	mkdir -p $(BIN_DIR)
	cp telemetry_user $(BIN_DIR)/

telemetry_user: telemetry_user.c
	$(CC) $(CFLAGS) -o telemetry_user telemetry_user.c

clean:
	rm -f telemetry_user
	rm -f $(BIN_DIR)/telemetry_user
