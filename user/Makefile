CC := gcc
CXX := g++
CFLAGS := -Wall -Wextra -pthread -I../include
CXXFLAGS := -Wall -Wextra -pthread -I../include
BIN_DIR := ../bin

.PHONY: all clean

all: telemetry_user block_filter_client
	mkdir -p $(BIN_DIR)
	cp telemetry_user $(BIN_DIR)/
	cp block_filter_client $(BIN_DIR)/

telemetry_user: telemetry_user.c
	$(CC) $(CFLAGS) -o telemetry_user telemetry_user.c telemetry_client.c net_filter_client.c

telemetry_client.o: telemetry_client.c
	$(CC) $(CFLAGS) -c telemetry_client.c

block_filter_client: block_filter_client.cpp telemetry_client.o
	$(CXX) $(CXXFLAGS) -o block_filter_client block_filter_client.cpp telemetry_client.o

clean:
	rm -f telemetry_user block_filter_client telemetry_client.o
	rm -f $(BIN_DIR)/telemetry_user
	rm -f $(BIN_DIR)/block_filter_client
