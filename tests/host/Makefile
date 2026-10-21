# SPDX-License-Identifier: GPL-3.0-only
CC ?= cc
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter
CPPFLAGS += -DXECHIDE_HOST -I../../kernel

SRC = test_main.c ../../kernel/xechide_match.c ../../kernel/xechide_parcel.c
FUZZ_SRC = test_fuzz.c ../../kernel/xechide_match.c ../../kernel/xechide_parcel.c
BIN = xechide-host-tests
FUZZ = xechide-host-fuzz
SANITIZE = -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all run fuzz clean

all: run fuzz

$(BIN): $(SRC) ../../kernel/xechide.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SRC)

run: $(BIN)
	./$(BIN)

$(FUZZ): $(FUZZ_SRC) ../../kernel/xechide.h
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter $(SANITIZE) -o $@ $(FUZZ_SRC)

fuzz: $(FUZZ)
	./$(FUZZ)

clean:
	rm -f $(BIN) $(FUZZ) *.pdb
