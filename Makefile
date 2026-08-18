CC ?= cc
OPTIMIZE ?= 0
UNAME_S := $(shell uname -s 2>/dev/null)

EXE :=
ifneq (,$(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)))
  EXE := .exe
endif
ifeq ($(OS),Windows_NT)
  EXE := .exe
endif

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
DEPFLAGS := -MMD -MP
LDFLAGS ?= $(SANFLAGS)

SRC := $(shell find src -name '*.c' | sort)
OBJ := $(patsubst %.c,build/%.o,$(SRC))
OBJ += build/flecs.o
OBJ += build/parson.o
DEP := $(OBJ:.o=.d)

BIN := build/bake$(EXE)
UNIT_BIN := build/bake_unit_tests$(EXE)
UNIT_OBJ := build/test/unit/unit_tests.o
UNIT_LIB_OBJ := $(filter-out build/src/main.o,$(OBJ))
DEP += $(UNIT_OBJ:.o=.d)
ifeq ($(UNAME_S),Linux)
  LDFLAGS += -pthread -lm
endif
ifeq ($(UNAME_S),Darwin)
  LDFLAGS += -pthread -lm
endif
ifneq (,$(filter MINGW% MSYS% CYGWIN%,$(UNAME_S)))
  LDFLAGS += -ldbghelp -lws2_32
endif
ifeq ($(OS),Windows_NT)
  LDFLAGS += -ldbghelp -lws2_32
endif

all: $(BIN)

build/flecs.o: deps/flecs.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -DFLECS_CUSTOM_BUILD -DFLECS_LOG -DFLECS_OS_API_IMPL -I deps -c $< -o $@

build/parson.o: deps/parson.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -I deps -c $< -o $@

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(DEPFLAGS) -I include -I deps -I src -c $< -o $@

$(BIN): $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(OBJ) $(LDFLAGS) -o $@

unit: $(UNIT_BIN)

$(UNIT_BIN): $(UNIT_OBJ) $(UNIT_LIB_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(UNIT_OBJ) $(UNIT_LIB_OBJ) $(LDFLAGS) -o $@

clean:
	rm -rf build

-include $(DEP)

.PHONY: all clean unit
