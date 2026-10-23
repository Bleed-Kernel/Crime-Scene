ELF_TARGET_64 = crime-scene
LIB_TARGET_64 = libcrime.a

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

CC 	:= x86_64-elf-gcc
AR 	:= x86_64-elf-ar
OBJCOPY := $(shell command -v x86_64-elf-objcopy >/dev/null 2>&1 && echo x86_64-elf-objcopy || echo objcopy)

ELF_BIN_DIR_64 = bin
ELF_OBJ_DIR_64 = $(ELF_BIN_DIR_64)/obj

LIB_DIR = sysroot/lib
LIBC = $(LIB_DIR)/blibc.a
CRT0 = $(LIB_DIR)/start.o

BLIBC_REPO = https://github.com/Bleed-Kernel/blibc.git
BLIBC_DIR  ?= $(if $(wildcard ../blibc/include/bleed),../blibc,external/blibc)

COMMON_CFLAGS = \
	-ffreestanding \
	-fno-stack-protector \
	-fno-stack-check \
	-Wall -Wextra \
	-O2 \
	-nostdlib \
	-no-pie \
	-m64 \
	-Isrc \
	-Iinclude \
	-Isysroot/include \
	-Isysroot/include/bleed \
	-nostdinc

LDFLAGS = \
	-static \
	-nostdlib \
	-no-pie \
	-m64 \
	-L$(LIB_DIR)

# src is the desktop and the terminal it puts the shell in, lib is what any program links to get a window.
# include only has what those programs need, the desktops own headers live in src with it
SRCS      = $(shell find src -name '*.c')
LIB_SRCS  = $(shell find lib -name '*.c')

ELF_OBJS_64  = $(patsubst %.c,$(ELF_OBJ_DIR_64)/%.o,$(SRCS))
LIB_OBJS_64  = $(patsubst %.c,$(ELF_OBJ_DIR_64)/%.o,$(LIB_SRCS))

LIBCRIME = $(ELF_BIN_DIR_64)/$(LIB_TARGET_64)

all: $(ELF_TARGET_64)

$(LIBCRIME): blibc $(LIB_OBJS_64)
	@mkdir -p $(ELF_BIN_DIR_64)
	$(AR) rcs $@ $(LIB_OBJS_64)

# the icon programs without their own get, same layout as a programs res/icon*.png
$(ELF_TARGET_64): blibc $(ELF_OBJS_64) $(LIBCRIME) $(wildcard res/default*.png)
	$(CC) $(LDFLAGS) \
		-o $(ELF_BIN_DIR_64)/$(ELF_TARGET_64) \
		$(CRT0) \
		$(ELF_OBJS_64) \
		$(LIBCRIME) \
		$(LIBC)
	@for f in res/default*.png; do \
		[ -f $$f ] || continue; \
		s=.bleed.$$(basename $$f .png | tr -d -); \
		$(OBJCOPY) --add-section $$s=$$f --set-section-flags $$s=noload,readonly $(ELF_BIN_DIR_64)/$(ELF_TARGET_64) || exit 1; \
	done

# -MMD writes out which headers each file used, so changing a header rebuilds everything
# that includes it. without that a struct can change under an object file thats never rebuilt
$(ELF_OBJ_DIR_64)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) -MMD -MP -c $< -o $@

-include $(ELF_OBJS_64:.o=.d) $(LIB_OBJS_64:.o=.d)

$(ELF_OBJS_64) $(LIB_OBJS_64): | blibc

blibc:
	@echo "[BLIBC] Preparing blibc"
	@if [ ! -d "$(BLIBC_DIR)" ]; then \
		git clone $(BLIBC_REPO) $(BLIBC_DIR); \
	elif [ "$(BLIBC_DIR)" = "external/blibc" ]; then \
		git -C $(BLIBC_DIR) pull --ff-only -q || echo "[BLIBC] couldnt pull, using what's there"; \
	fi
	$(MAKE) -C $(BLIBC_DIR)

	@echo "[BLIBC] Syncing sysroot"
	@mkdir -p sysroot
	@cp -r $(BLIBC_DIR)/sysroot/* sysroot/

build: all

clean:
	rm -rf bin

distclean:
	rm -rf bin sysroot external

.PHONY: all build clean distclean blibc $(ELF_TARGET_64)
