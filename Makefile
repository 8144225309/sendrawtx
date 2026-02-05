# RawRelay Server v6 - Multi-Process Architecture with TLS/HTTP2
# Makefile for building and testing

CC = gcc
CFLAGS = -Wall -Wextra -Werror -g -O2 -I./include -D_GNU_SOURCE
LDFLAGS = -levent -levent_pthreads -levent_openssl -lssl -lcrypto -lnghttp2 -lm

SRC_DIR = src
BUILD_DIR = build
TEST_DIR = tests
INCLUDE_DIR = include

# Source files for v6 server (with TLS and HTTP/2)
SRCS = $(SRC_DIR)/main.c \
       $(SRC_DIR)/master.c \
       $(SRC_DIR)/worker.c \
       $(SRC_DIR)/connection.c \
       $(SRC_DIR)/config.c \
       $(SRC_DIR)/log.c \
       $(SRC_DIR)/tcp_opts.c \
       $(SRC_DIR)/buffer.c \
       $(SRC_DIR)/reader.c \
       $(SRC_DIR)/http.c \
       $(SRC_DIR)/router.c \
       $(SRC_DIR)/static_files.c \
       $(SRC_DIR)/slot_manager.c \
       $(SRC_DIR)/rate_limiter.c \
       $(SRC_DIR)/ip_acl.c \
       $(SRC_DIR)/tls.c \
       $(SRC_DIR)/http2.c

OBJS = $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

# Main target
TARGET = rawrelay-server

# Object files for tests (without main)
LIB_OBJS = $(BUILD_DIR)/buffer.o $(BUILD_DIR)/config.o $(BUILD_DIR)/reader.o $(BUILD_DIR)/http.o $(BUILD_DIR)/log.o

.PHONY: all clean test test_buffer test_reader valgrind help check-libevent

all: check-libevent $(TARGET)

# Check for dependencies
check-libevent:
	@pkg-config --exists libevent 2>/dev/null || (echo "ERROR: libevent not found. Install with: sudo apt install libevent-dev" && exit 1)
	@pkg-config --exists openssl 2>/dev/null || (echo "ERROR: OpenSSL not found. Install with: sudo apt install libssl-dev" && exit 1)
	@pkg-config --exists libnghttp2 2>/dev/null || (echo "ERROR: nghttp2 not found. Install with: sudo apt install libnghttp2-dev" && exit 1)

# Main server executable
$(TARGET): $(OBJS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo ""
	@echo "Build successful!"
	@echo "Run with: ./$(TARGET)"
	@echo "Or test with: ./$(TARGET) -t"

# Object files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# Create build directory
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Individual tests
test_buffer: $(BUILD_DIR)/buffer.o $(BUILD_DIR)/log.o $(TEST_DIR)/test_buffer.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $(BUILD_DIR)/$@ $(BUILD_DIR)/buffer.o $(BUILD_DIR)/log.o $(TEST_DIR)/test_buffer.c $(LDFLAGS)
	./$(BUILD_DIR)/$@

test_reader: $(LIB_OBJS) $(TEST_DIR)/test_reader.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -o $(BUILD_DIR)/$@ $(LIB_OBJS) $(TEST_DIR)/test_reader.c $(LDFLAGS)
	./$(BUILD_DIR)/$@

# Run all tests
test: test_buffer test_reader

# Memory check with valgrind
valgrind: $(TARGET)
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./$(TARGET) -t

valgrind_run: $(TARGET)
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --trace-children=yes ./$(TARGET) -w 1

# Quick single-worker test (easier to debug)
run1: $(TARGET)
	./$(TARGET) -w 1

# Run with default workers
run: $(TARGET)
	./$(TARGET)

# Clean build artifacts
clean:
	rm -rf $(BUILD_DIR) $(TARGET)

# Install dependencies (Ubuntu/Debian)
deps:
	sudo apt update
	sudo apt install -y build-essential libevent-dev libssl-dev libnghttp2-dev pkg-config valgrind curl

# Help
help:
	@echo "RawRelay Server v6 - Multi-Process Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all              Build the server (default)"
	@echo "  run              Run server with auto-detected workers"
	@echo "  run1             Run server with 1 worker (for debugging)"
	@echo "  test             Run all unit tests"
	@echo "  test_buffer      Run buffer module tests"
	@echo "  test_reader      Run reader module tests"
	@echo "  valgrind         Run config test with valgrind"
	@echo "  valgrind_run     Run server with valgrind (1 worker)"
	@echo "  deps             Install build dependencies (apt)"
	@echo "  clean            Remove build artifacts"
	@echo "  help             Show this help message"
	@echo ""
	@echo "Usage:"
	@echo "  make              # Build the server"
	@echo "  make run          # Run with auto workers"
	@echo "  make run1         # Run with 1 worker"
	@echo "  ./rawrelay-server -h   # Show server help"
	@echo ""
	@echo "Testing:"
	@echo "  curl http://localhost:8080/test   # Test connection"
	@echo "  kill -HUP <pid>                   # Graceful reload"
	@echo "  kill -TERM <pid>                  # Graceful shutdown"
