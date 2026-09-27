#----------------------------------------------------------------------------
# Rise of the Triad on the PS3 (Taradino + PSL1GHT)
#
# From the root of the tree (~/ps3-test/rott):
#
#   make -C ps3                -> ps3/rott-ps3-1.0.pkg
#   make -C ps3 clean
#   make -C ps3 check-toolchain
#
# One EBOOT, two engines: Taradino builds the registered game (Dark War,
# Extreme ROTT) and the shareware one (The HUNT Begins) from the
# same sources with SHAREWARE=0/1. Both are compiled here and linked in,
# each squeezed into one object whose only global symbol is its main()
# (renamed ROTT_Main_Registered / ROTT_Main_Shareware), like the game DLLs
# of the Quake II port (handoff notes 3.51). The launcher picks one.
#
# The engine's sources are untouched but for #ifdef __PS3__ blocks; what
# it calls from the C library that must behave differently on the PS3
# (exit, printf and friends, file functions taking paths) is renamed in
# its objects to the PS3_* versions (ps3/source/ps3_stdio.c).
#
# Packaging (.elf -> .self -> .pkg) is done by ppu_rules' own pattern rules
# (strip + sprxlinker + sign + package_finalize). Never by hand.
#----------------------------------------------------------------------------

ifeq ($(strip $(PS3DEV)),)
$(error PS3DEV is not set. Do: export PS3DEV=/usr/local/ps3dev)
endif

include $(PS3DEV)/ppu_rules

#----------------------------------------------------------------------------
# Package identity
#
# APPID: EXACTLY 9 characters. ':=' and not '?=': ppu_rules sets
# TITLE/APPID with '?=' BEFORE these lines (handoff notes 3.15, 3.16).
#----------------------------------------------------------------------------

VERSION   := 1.0
TARGET    := rott-ps3
TITLE     := Rise of the Triad
APPID     := RISETRIAD
CONTENTID := UP0001-$(APPID)_00-0000000000000000
PKGFILES  := $(CURDIR)/pkgfiles
ICON0     := $(CURDIR)/ICON0.PNG
OUT_PKG   := rott-ps3-$(VERSION).pkg

SRC       := $(abspath $(CURDIR)/..)
OBJDIR    := $(CURDIR)/obj
BUILDDIR  := $(CURDIR)/build

CC        := ppu-gcc
LD        := ppu-gcc
PARTIALLD := ppu-ld
OBJCOPY   := ppu-objcopy

#----------------------------------------------------------------------------
# Flags
#----------------------------------------------------------------------------

CFLAGS := -O2 -std=gnu99 -Wall -fno-strict-aliasing -fno-common
CFLAGS += -Wno-unused-variable -Wno-unused-function -Wno-unused-but-set-variable
CFLAGS += -Wno-format-truncation -Wno-misleading-indentation
# gcc 8 and later (older ones print "unrecognized command line option")
CFLAGS += $(shell $(CC) -Wstringop-truncation -E -x c /dev/null >/dev/null 2>&1 && echo -Wno-stringop-truncation)

# PPU is LP64: a missing prototype truncates pointers and 64-bit values to
# int. Make those hard errors (handoff notes 3.26).
CFLAGS += -Werror=implicit-function-declaration -Werror=int-conversion
CFLAGS += -Werror=pointer-to-int-cast -Werror=int-to-pointer-cast
CFLAGS += -MMD -MP

CFLAGS += -D__PS3__ -DPS3_APPID=\"$(APPID)\" -DBUILD_DATE=\"$(shell date +%Y-%m-%d)\"
CFLAGS += -I$(CURDIR)/compat -I$(CURDIR)/source
CFLAGS += $(LIBPSL1GHT_INC)

# The engine: 1994 DOS code written on x86, where char is signed (on the
# PPU it isn't: handoff notes 3.27). Its own defines, as CMake sets them.
ENGINE_CFLAGS := $(CFLAGS) -fsigned-char -I$(OBJDIR)/include -I$(SRC)/rott
# the warnings left are all the original code's ("may be used
# uninitialized", char array indexes, overlapping strcpy)
ENGINE_CFLAGS += -Wno-maybe-uninitialized -Wno-char-subscripts
ENGINE_CFLAGS += $(shell $(CC) -Wrestrict -E -x c /dev/null >/dev/null 2>&1 && echo -Wno-restrict)
ENGINE_CFLAGS += -DHAVE_DIRENT_H -DPACKAGE_TARNAME=\"taradino\"
ENGINE_CFLAGS += -DPACKAGE_STRING="\"Rise of the Triad (PS3) $(VERSION)\""

# -lrt and -llv2 are MANDATORY: without them the binary links cleanly, the
# PKG installs, and it never reaches main(). Black screen, back to the XMB,
# no log at all (handoff notes 3.1).
LIBS := gcm_sys rsx sysutil io audio rt lv2 m

#----------------------------------------------------------------------------
# Sources
#----------------------------------------------------------------------------

ENGINE_SRCS := $(sort $(wildcard $(SRC)/rott/*.c))
ENGINE_REG  := $(patsubst $(SRC)/rott/%.c,$(OBJDIR)/reg/%.o,$(ENGINE_SRCS))
ENGINE_SW   := $(patsubst $(SRC)/rott/%.c,$(OBJDIR)/sw/%.o,$(ENGINE_SRCS))

PLATFORM_SRCS := ps3_main.c ps3_log.c ps3_stdio.c ps3_pad.c ps3_input.c \
	ps3_video.c ps3_launcher.c sdl_ps3.c sdl_mixer_ps3.c ps3_midi.c \
	ps3_timbre.c opl3.c ps3_sf2.c
PLATFORM_OBJS := $(patsubst %.c,$(OBJDIR)/platform/%.o,$(PLATFORM_SRCS))

# The C library calls the engine makes that go to the PS3 versions.
REDEFINE := exit printf fprintf vfprintf vprintf puts putchar fputs fputc \
	putc fwrite fflush perror abort open fopen stat access mkdir opendir \
	remove unlink rename getcwd chdir
REDEFINE_FLAGS := $(foreach s,$(REDEFINE),--redefine-sym $(s)=PS3_$(s))

# The file that talks to GCM goes at -O1 -fno-inline: PSL1GHT's syscall
# stubs don't survive aggressive inlining (handoff notes 4.2).
$(OBJDIR)/platform/ps3_video.o: CFLAGS := $(filter-out -O2,$(CFLAGS)) -O1 -fno-inline

#----------------------------------------------------------------------------
# Rules
#----------------------------------------------------------------------------

.PHONY: all clean check-toolchain pkgdirs FORCE

all: $(TARGET).pkg
	@cp $(TARGET).gnpdrm.pkg $(OUT_PKG)
	@echo ""
	@echo "PKG to install: $(CURDIR)/$(OUT_PKG)"
	@ls -la $(OUT_PKG)

# version.h, as CMake's configure_file writes it
$(OBJDIR)/include/version.h: $(SRC)/rott/version.h.in
	@mkdir -p $(dir $@)
	@sed 's/@CMAKE_PROJECT_VERSION@/$(VERSION)/' $< > $@

$(OBJDIR)/reg/%.o: $(SRC)/rott/%.c $(OBJDIR)/include/version.h
	@mkdir -p $(dir $@)
	@echo "CC  [dark war] $*.c"
	@$(CC) $(ENGINE_CFLAGS) -c $< -o $@

$(OBJDIR)/sw/%.o: $(SRC)/rott/%.c $(OBJDIR)/include/version.h
	@mkdir -p $(dir $@)
	@echo "CC  [shareware] $*.c"
	@$(CC) $(ENGINE_CFLAGS) -DSHAREWARE=1 -c $< -o $@

$(OBJDIR)/platform/%.o: $(CURDIR)/source/%.c
	@mkdir -p $(dir $@)
	@echo "CC  $*.c"
	@$(CC) $(CFLAGS) -c $< -o $@

# One engine = one object, only its main() stays global.
$(OBJDIR)/engine_reg.o: $(ENGINE_REG)
	@echo "LD  engine_reg.o (only ROTT_Main_Registered stays global)"
	@$(PARTIALLD) -r -o $(OBJDIR)/reg_all.o $(ENGINE_REG)
	@$(OBJCOPY) --redefine-sym main=ROTT_Main_Registered $(REDEFINE_FLAGS) \
		$(OBJDIR)/reg_all.o $(OBJDIR)/reg_renamed.o
	@$(OBJCOPY) --keep-global-symbol=ROTT_Main_Registered $(OBJDIR)/reg_renamed.o $@

$(OBJDIR)/engine_sw.o: $(ENGINE_SW)
	@echo "LD  engine_sw.o (only ROTT_Main_Shareware stays global)"
	@$(PARTIALLD) -r -o $(OBJDIR)/sw_all.o $(ENGINE_SW)
	@$(OBJCOPY) --redefine-sym main=ROTT_Main_Shareware $(REDEFINE_FLAGS) \
		$(OBJDIR)/sw_all.o $(OBJDIR)/sw_renamed.o
	@$(OBJCOPY) --keep-global-symbol=ROTT_Main_Shareware $(OBJDIR)/sw_renamed.o $@

$(TARGET).elf: $(OBJDIR)/engine_reg.o $(OBJDIR)/engine_sw.o $(PLATFORM_OBJS)
	@echo "LD  $@"
	@$(LD) $^ $(LIBPSL1GHT_LIB) -o $@ $(foreach lib,$(LIBS),-l$(lib))

-include $(ENGINE_REG:.o=.d) $(ENGINE_SW:.o=.d) $(PLATFORM_OBJS:.o=.d)

#----------------------------------------------------------------------------
# PKG contents and icon
#
# pkgfiles/ mirrors the PKG, not its USRDIR. VERSION.TXT is always written
# (it also tells which build is installed). Game data is NOT packaged: it
# goes by FTP into USRDIR. (Mods are for 1.1: see ps3_launcher.c.)
#----------------------------------------------------------------------------

$(PKGFILES)/USRDIR/VERSION.TXT: FORCE
	@mkdir -p $(PKGFILES)/USRDIR
	@echo "$(TITLE) -- rott-ps3 $(VERSION) -- `date +%Y-%m-%d`" > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv -f $@.tmp $@; fi

$(ICON0):
	@echo "No ICON0.PNG, generating a 320x176 placeholder..."
	@python3 -c "import zlib,struct;w,h=320,176;\
rows=b''.join(b'\x00'+bytes([90,20,16,255])*w for _ in range(h));\
png=lambda t,d:struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d));\
open('$(ICON0)','wb').write(b'\x89PNG\r\n\x1a\n'+png(b'IHDR',struct.pack('>IIBBBBB',w,h,8,6,0,0,0))+png(b'IDAT',zlib.compress(rows))+png(b'IEND',b''))"

# The folders the user fills. Created at build time: they are empty, and
# neither git nor a patch keeps empty folders. pkg packs them too.
PKG_DIRS :=

pkgdirs:
	@for d in $(PKG_DIRS); do mkdir -p "$(PKGFILES)/$$d"; done

$(TARGET).pkg: $(PKGFILES)/USRDIR/VERSION.TXT $(ICON0) | pkgdirs

#----------------------------------------------------------------------------
# Diagnostics
#----------------------------------------------------------------------------

check-toolchain:
	@echo "PS3DEV   = $(PS3DEV)"
	@echo "ppu_rules: `test -f $(PS3DEV)/ppu_rules && echo OK || echo MISSING`"
	@$(CC) --version | head -1
	@echo "APPID    = $(APPID)  (length: `printf '%s' '$(APPID)' | wc -c`, MUST be 9)"
	@echo "TITLE    = $(TITLE)"
	@for l in $(LIBS); do \
	   if [ -f "$(PS3DEV)/ppu/lib/lib$$l.a" ] || [ -f "$(PSL1GHT)/ppu/lib/lib$$l.a" ]; then \
	     printf "  OK      lib%s.a\n" "$$l"; \
	   elif $(CC) -print-file-name=lib$$l.a | grep -q /; then \
	     printf "  OK      lib%s.a (gcc sysroot)\n" "$$l"; \
	   else \
	     printf "  MISSING lib%s.a\n" "$$l"; \
	   fi; \
	 done

clean:
	rm -rf $(OBJDIR) $(BUILDDIR)
	rm -f $(TARGET).elf $(TARGET).self $(TARGET).fake.self
	rm -f $(PKGFILES)/USRDIR/VERSION.TXT
	@echo "clean (the .pkg is left alone: ppu_rules rebuilds it)"
