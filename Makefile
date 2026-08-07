# ============================================================================
#  PWRadarSystem - plain GNU make build for Linux
#  --------------------------------------------------------------------------
#  Provided as a no-configure alternative to CMake for anyone who just wants
#  `make && ./build/PWRadarUI`.  Both produce identical binaries.
#
#      make            release build
#      make debug      -O0 -g with the sanitizers available
#      make test       build and run the core acceptance suite
#      make clean
# ============================================================================

CC      ?= cc
BUILD   ?= build
CORE_SRC := $(wildcard PWRadarCore/src/*.c)
UI_SRC   := $(wildcard PWRadarUI/src/*.c)
CORE_OBJ := $(CORE_SRC:%.c=$(BUILD)/obj/%.o)
UI_OBJ   := $(UI_SRC:%.c=$(BUILD)/obj/%.o)

CORE_LIB := $(BUILD)/libPWRadarCore.so
UI_BIN   := $(BUILD)/PWRadarUI

INCLUDES := -IPWRadarCore/include -IPWRadarCore/src -IPWRadarUI/src
WARN     := -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
            -Wstrict-prototypes -Wmissing-prototypes -Wcast-qual -Wpointer-arith
STD      := -std=c17
OPT      ?= -O2 -fno-math-errno
CFLAGS   += $(STD) $(OPT) $(WARN) $(INCLUDES) -fPIC -MMD -MP
LDLIBS_CORE := -lm -lpthread
LDLIBS_UI   := -lX11 -lm

.PHONY: all debug asan test clean run

all: $(UI_BIN)

debug: OPT := -O0 -g3 -fno-omit-frame-pointer
debug: clean $(UI_BIN)

asan: OPT := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
asan: LDLIBS_CORE += -fsanitize=address,undefined
asan: LDLIBS_UI   += -fsanitize=address,undefined

asan: clean $(UI_BIN)

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DPWR_BUILD_SHARED -c $< -o $@

# No -soname here: the build tree holds a single unversioned object that the
# console finds through its $ORIGIN rpath.  Versioned SONAMEs and symlinks are
# the installer's job and CMake handles them.
$(CORE_LIB): $(CORE_OBJ)
	@mkdir -p $(dir $@)
	$(CC) -shared $(CORE_OBJ) $(LDLIBS_CORE) -o $@

# The console binds to the shared object with an $ORIGIN rpath, so it runs from
# the build directory without LD_LIBRARY_PATH being set.
$(UI_BIN): $(UI_OBJ) $(CORE_LIB)
	$(CC) $(UI_OBJ) -L$(BUILD) -lPWRadarCore $(LDLIBS_UI) \
	      -Wl,-rpath,'$$ORIGIN' -o $@

test: $(UI_BIN)
	$(UI_BIN) --selftest

run: $(UI_BIN)
	$(UI_BIN)

clean:
	rm -rf $(BUILD)

-include $(CORE_OBJ:.o=.d) $(UI_OBJ:.o=.d)
