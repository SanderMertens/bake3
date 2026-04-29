CC ?= cc
OPTIMIZE ?= 0
UNAME_S := $(shell uname -s 2>/dev/null)

ifeq ($(OPTIMIZE),1)
  OPTFLAGS ?= -O3
else
  OPTFLAGS ?= -g
endif

SANITIZE ?= 0
ifeq ($(SANITIZE),1)
  SANFLAGS := -fsanitize=address -fno-omit-frame-pointer
else
  SANFLAGS :=
endif

CSTD ?= -std=gnu99

CFLAGS ?= $(CSTD) -Wall -Wextra -Werror -pedantic $(OPTFLAGS) $(SANFLAGS)
LDFLAGS ?= $(SANFLAGS)

SRC := $(shell find src -name '*.c' | sort)
OBJ := $(patsubst %.c,build/%.o,$(SRC))
OBJ += build/flecs.o
OBJ += build/parson.o

BIN := build/bake
ifeq ($(UNAME_S),Linux)
  LDFLAGS += -pthread -lm
endif
ifeq ($(UNAME_S),Darwin)
  LDFLAGS += -pthread -lm
endif

all: $(BIN)

build/flecs.o: deps/flecs.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS)  -DFLECS_CUSTOM_BUILD -DFLECS_LOG -DFLECS_OS_API_IMPL -I deps -c $< -o $@

build/parson.o: deps/parson.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I deps -c $< -o $@

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I include -I deps -I src -c $< -o $@

$(BIN): $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(OBJ) $(LDFLAGS) -o $@

clean:
	rm -rf build

.PHONY: all clean
