CC ?= cc
OPTIMIZE ?= 0
ifeq ($(OPTIMIZE),1)
  OPTFLAGS ?= -O3
else
  OPTFLAGS ?= -g
endif

CFLAGS ?= -std=c99 -Wall -Wextra -Werror -pedantic $(OPTFLAGS)
LDFLAGS ?=

SRC := $(shell find src -name '*.c' | sort)
OBJ := $(patsubst %.c,build/%.o,$(SRC))
OBJ += build/flecs.o
OBJ += build/parson.o

BIN := build/bake

UNAME_S := $(shell uname -s 2>/dev/null)
ifeq ($(UNAME_S),Linux)
  LDFLAGS += -pthread -lm
endif
ifeq ($(UNAME_S),Darwin)
  LDFLAGS += -pthread -lm
endif

all: $(BIN)

build/flecs.o: deps/flecs.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I deps -c $< -o $@

build/parson.o: deps/parson.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I deps -c $< -o $@

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I include -I deps -c $< -o $@

$(BIN): $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(OBJ) $(LDFLAGS) -o $@

clean:
	rm -rf build

.PHONY: all clean
