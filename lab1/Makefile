CC      := clang
CFLAGS  := -std=c99 -Wall -pedantic -Werror
SRC_DIR := source
BIN     := pa1
SRCS    := $(wildcard $(SRC_DIR)/*.c)
HDRS    := $(wildcard $(SRC_DIR)/*.h)

P ?= 2

RESULT_DIR := result
STAGE_DIR  := $(RESULT_DIR)/pa1
ARCHIVE    := $(RESULT_DIR)/pa1.tar.gz

.PHONY: all run clean pack

all: $(BIN)

$(BIN): $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

run: $(BIN)
	./$(BIN) -p $(P)

clean:
	rm -f $(BIN) events.log pipes.log
	rm -rf $(RESULT_DIR)

# Archive to send
pack: $(BIN)
	rm -rf $(STAGE_DIR)
	mkdir -p $(STAGE_DIR)
	cp $(SRCS) $(HDRS) $(STAGE_DIR)/
	tar -czf $(ARCHIVE) -C $(RESULT_DIR) pa1
	rm -rf $(STAGE_DIR)
