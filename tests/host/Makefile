CC ?= cc
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter
CPPFLAGS += -DXECHIDE_HOST -I../../kernel

SRC = test_main.c ../../kernel/xechide_match.c ../../kernel/xechide_parcel.c
BIN = xechide-host-tests

.PHONY: all run clean

all: run

$(BIN): $(SRC) ../../kernel/xechide.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SRC)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)
