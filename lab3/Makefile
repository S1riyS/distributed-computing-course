CC      := clang
CFLAGS  := -std=c99 -Wall -pedantic -Werror
SRC_DIR := source
LIB_DIR := $(SRC_DIR)/lib64
RUNTIME := $(LIB_DIR)/libruntime.so
LDFLAGS := -L$(LIB_DIR) -lruntime
BIN     := pa3
SRCS    := $(wildcard $(SRC_DIR)/*.c)
HDRS    := $(wildcard $(SRC_DIR)/*.h)

P ?= 2
BALANCES ?= 10 20

RESULT_DIR := result
STAGE_DIR  := $(RESULT_DIR)/pa3
ARCHIVE    := $(RESULT_DIR)/pa3.tar.gz

.PHONY: all run clean pack

all: $(BIN)

$(BIN): $(SRCS) $(HDRS) $(RUNTIME)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

# Запуск из корня lab3. Число счетов и балансы те же, что в ЛР 2:
# make run P=3 BALANCES="10 20 30"
run: $(BIN)
	LD_LIBRARY_PATH=$(abspath $(LIB_DIR)):$$LD_LIBRARY_PATH \
	LD_PRELOAD=$(abspath $(RUNTIME)) \
	./$(BIN) -p $(P) $(BALANCES)

clean:
	rm -f $(BIN) events.log pipes.log
	rm -rf $(RESULT_DIR)

# Архив для отправки: result/pa3.tar.gz, внутри каталог pa3 с *.c и *.h
pack: $(BIN)
	rm -rf $(STAGE_DIR)
	mkdir -p $(STAGE_DIR)
	cp $(SRCS) $(HDRS) $(STAGE_DIR)/
	tar -czf $(ARCHIVE) -C $(RESULT_DIR) pa3
	rm -rf $(STAGE_DIR)
