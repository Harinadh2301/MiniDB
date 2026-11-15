CC = gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -g
CPPFLAGS ?= -Iinclude
LDFLAGS ?=
SOURCES := $(wildcard src/*.c)
LIB_SOURCES := $(filter-out src/main.c,$(SOURCES))
ifeq ($(OS),Windows_NT)
APP_BIN := minidb.exe
TEST_BIN := tests/test_minidb.exe
else
APP_BIN := minidb
TEST_BIN := tests/test_minidb
endif

.PHONY: all build test script-test debug bench clean

all: build

build: $(APP_BIN)

$(APP_BIN): $(SOURCES) $(wildcard include/*.h)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(LDFLAGS) -o $@

$(TEST_BIN): tests/test_minidb.c $(LIB_SOURCES) $(wildcard include/*.h)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LIB_SOURCES) tests/test_minidb.c $(LDFLAGS) -o $@

test: $(APP_BIN) $(TEST_BIN)
	./$(TEST_BIN)
	python tests/run_regression.py

script-test: $(APP_BIN)
	python tests/run_regression.py

debug:
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Wpedantic -g -DBT_ORDER=4 \
		-fsanitize=address,undefined $(SOURCES) -o minidb_debug

bench:
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Wpedantic -O2 \
		-DBT_ORDER=64 -DNDEBUG src/bptree.c tests/benchmark.c -o minidb_bench
	./minidb_bench

ifeq ($(OS),Windows_NT)
clean:
	-cmd /c del /Q minidb.exe minidb_debug.exe minidb_bench.exe tests\test_minidb.exe
else
clean:
	$(RM) minidb minidb.exe minidb_debug minidb_debug.exe \
		minidb_bench minidb_bench.exe $(TEST_BIN) $(TEST_BIN).exe
endif
