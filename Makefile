CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -Werror -pedantic -g
LDFLAGS ?=

SRC := $(shell find src -name '*.c' | sort)
OBJ := $(patsubst %.c,build/%.o,$(SRC))
OBJ += build/flecs.o

BIN := build/bake

UNAME_S := $(shell uname -s 2>/dev/null)
ifeq ($(UNAME_S),Linux)
  LDFLAGS += -pthread -lm
endif
ifeq ($(UNAME_S),Darwin)
  LDFLAGS += -pthread -lm
endif

all: $(BIN)

build/flecs.o: flecs/distr/flecs.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I flecs/distr -c $< -o $@

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I include -I flecs/distr -c $< -o $@

$(BIN): $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(OBJ) $(LDFLAGS) -o $@

clean:
	rm -rf build

.PHONY: all clean
