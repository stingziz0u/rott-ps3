#----------------------------------------------------------------------------
# Host harness (see host_ps3.c). Not needed to build the PKG.
#
#   make -C ps3/hosttest              x86-64, native
#   make -C ps3/hosttest ARCH=ppc64   powerpc64 big-endian (run it with
#                                     qemu-ppc64): the PS3's byte order
#
# USRDIR (game data) and DATADIR (config, saves) are baked in:
#   make -C ps3/hosttest USRDIR=/path/to/data DATADIR=/path/to/saves
#
# Then:
#   ROTT_HOST_OUT=/tmp/shots ROTT_HOST_SHOTS=5000,9000 \
#   ROTT_HOST_PAD="4000:CROSS,4300:" ROTT_HOST_QUIT_MS=20000 \
#   qemu-ppc64 ps3/hosttest/rott-host-ppc64
#----------------------------------------------------------------------------

ARCH    ?= x86
USRDIR  ?= /tmp/rott-usrdir
DATADIR ?= /tmp/rott-data

ifeq ($(ARCH),ppc64)
CC      := powerpc64-linux-gnu-gcc
LD      := powerpc64-linux-gnu-ld
OBJCOPY := powerpc64-linux-gnu-objcopy
LDFLAGS := -static
else
CC      := gcc
LD      := ld
OBJCOPY := objcopy
LDFLAGS :=
endif

HERE    := $(abspath $(CURDIR))
PS3     := $(abspath $(HERE)/..)
SRC     := $(abspath $(PS3)/..)
OBJDIR  := $(HERE)/obj-$(ARCH)
TARGET  := $(HERE)/rott-host-$(ARCH)

CFLAGS := -O1 -g -std=gnu99 -Wall -fno-strict-aliasing -fno-common
CFLAGS += -Wno-unused-variable -Wno-unused-function -Wno-unused-but-set-variable
CFLAGS += -Wno-format-truncation -Wno-stringop-truncation -Wno-misleading-indentation
CFLAGS += -Wno-maybe-uninitialized
CFLAGS += -Werror=implicit-function-declaration -Werror=int-conversion
CFLAGS += -Werror=pointer-to-int-cast -Werror=int-to-pointer-cast
CFLAGS += -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0
CFLAGS += -D__PS3__ -DBUILD_DATE=\"host\"
CFLAGS += -DPS3_USRDIR=\"$(USRDIR)\" -DPS3_DATADIR=\"$(DATADIR)\"
CFLAGS += -I$(PS3)/compat -I$(PS3)/source

ENGINE_CFLAGS := $(CFLAGS) -fsigned-char -I$(OBJDIR)/include -I$(SRC)/rott
ENGINE_CFLAGS += -DHAVE_DIRENT_H -DPACKAGE_TARNAME=\"taradino\"
ENGINE_CFLAGS += -DPACKAGE_STRING="\"Rise of the Triad (host)\""

ENGINE_SRCS := $(sort $(wildcard $(SRC)/rott/*.c))
ENGINE_REG  := $(patsubst $(SRC)/rott/%.c,$(OBJDIR)/reg/%.o,$(ENGINE_SRCS))
ENGINE_SW   := $(patsubst $(SRC)/rott/%.c,$(OBJDIR)/sw/%.o,$(ENGINE_SRCS))

PLATFORM_SRCS := ps3_main.c ps3_log.c ps3_stdio.c ps3_input.c ps3_launcher.c \
	sdl_ps3.c sdl_mixer_ps3.c ps3_midi.c ps3_timbre.c opl3.c ps3_sf2.c
PLATFORM_OBJS := $(patsubst %.c,$(OBJDIR)/platform/%.o,$(PLATFORM_SRCS)) \
	$(OBJDIR)/platform/host_ps3.o

REDEFINE := exit printf fprintf vfprintf vprintf puts putchar fputs fputc \
	putc fwrite fflush perror abort open fopen stat access mkdir opendir \
	remove unlink rename getcwd chdir
REDEFINE_FLAGS := $(foreach s,$(REDEFINE),--redefine-sym $(s)=PS3_$(s))

.PHONY: all clean

all: $(TARGET)

$(OBJDIR)/include/version.h: $(SRC)/rott/version.h.in
	@mkdir -p $(dir $@)
	@sed 's/@CMAKE_PROJECT_VERSION@/host/' $< > $@

$(OBJDIR)/reg/%.o: $(SRC)/rott/%.c $(OBJDIR)/include/version.h
	@mkdir -p $(dir $@)
	@$(CC) $(ENGINE_CFLAGS) -c $< -o $@

$(OBJDIR)/sw/%.o: $(SRC)/rott/%.c $(OBJDIR)/include/version.h
	@mkdir -p $(dir $@)
	@$(CC) $(ENGINE_CFLAGS) -DSHAREWARE=1 -c $< -o $@

$(OBJDIR)/platform/%.o: $(PS3)/source/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/platform/host_ps3.o: $(HERE)/host_ps3.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/engine_reg.o: $(ENGINE_REG)
	@$(LD) -r -o $(OBJDIR)/reg_all.o $(ENGINE_REG)
	@$(OBJCOPY) --redefine-sym main=ROTT_Main_Registered $(REDEFINE_FLAGS) $(OBJDIR)/reg_all.o $(OBJDIR)/reg_renamed.o
	@$(OBJCOPY) --keep-global-symbol=ROTT_Main_Registered $(OBJDIR)/reg_renamed.o $@

$(OBJDIR)/engine_sw.o: $(ENGINE_SW)
	@$(LD) -r -o $(OBJDIR)/sw_all.o $(ENGINE_SW)
	@$(OBJCOPY) --redefine-sym main=ROTT_Main_Shareware $(REDEFINE_FLAGS) $(OBJDIR)/sw_all.o $(OBJDIR)/sw_renamed.o
	@$(OBJCOPY) --keep-global-symbol=ROTT_Main_Shareware $(OBJDIR)/sw_renamed.o $@

$(TARGET): $(OBJDIR)/engine_reg.o $(OBJDIR)/engine_sw.o $(PLATFORM_OBJS)
	@$(CC) $(LDFLAGS) $^ -o $@ -lpthread -lm
	@echo "built $@"

clean:
	rm -rf $(HERE)/obj-x86 $(HERE)/obj-ppc64 $(HERE)/rott-host-x86 $(HERE)/rott-host-ppc64
