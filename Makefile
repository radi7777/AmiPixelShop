PATH := $(HOME)/opt/m68k-amigaos-gcc-16.2/bin:$(PATH)
CC      = m68k-amigaos-gcc
CFLAGS  = -noixemul -O3 -m68040 -m68881 -fno-math-errno -fomit-frame-pointer -Wall -Wno-pointer-sign -Wno-stringop-truncation
LDLIBS  = -lamiga -lm

BUILD   = build
SRCS    = src/main.c src/gfx.c src/pixel.c src/doc.c src/view.c src/paint.c src/select.c src/ui.c src/fileio.c src/stb_impl.c src/move.c src/filter.c src/bench.c src/clip.c src/transform.c src/text.c src/resize.c src/tools.c src/fx.c src/crop.c src/perf.c src/psd.c src/path.c src/liquify.c src/quant.c src/net.c src/ai.c src/update.c src/maglasso.c src/guides.c
OBJS    = $(SRCS:src/%.c=$(BUILD)/%.o)
AOBJS   = $(BUILD)/pixel_asm.o
VASM    = vasmm68k_mot
ZSRCS   = adler32 compress crc32 deflate inffast inflate inftrees trees uncompr zutil
ZOBJS   = $(ZSRCS:%=$(BUILD)/z_%.o)
LIBFLAGS = -noixemul -O2 -m68040 -m68881 -fno-math-errno -fomit-frame-pointer -w

all: $(BUILD)/AmiPixelShop $(BUILD)/proto0

# Linken mit -mcpu=68020 -m68881: nur so waehlt gcc die Mathe-Bibliothek, die die
# FPU direkt nutzt (libm020/libm881). Mit -m68040 kaemen floorf & Co. ueber die
# Amiga-Mathe-Libraries, und die zerstoeren die FPU-Register fp2-fp7.
LDFLAGS = -noixemul -mcpu=68020 -m68881

$(BUILD)/AmiPixelShop: $(OBJS) $(ZOBJS) $(AOBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/pixel_asm.o: src/pixel_asm.s
	@mkdir -p $(BUILD)
	$(VASM) -Fhunk -m68040 -quiet -o $@ $<

$(BUILD)/z_%.o: third_party/zlib/%.c
	@mkdir -p $(BUILD)
	$(CC) $(LIBFLAGS) -c -o $@ $<

$(BUILD)/%.o: src/%.c src/apx.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc -c -o $@ $<

$(BUILD)/gfx.o: src/font_ui.h src/font_bold.h src/font_small.h

STB     = -Ithird_party/stb -Ithird_party/zlib
$(BUILD)/text.o: src/text.c src/apx.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc $(STB) -c -o $@ $<

$(BUILD)/ai.o: src/ai.c src/apx.h src/net.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc $(STB) -c -o $@ $<

$(BUILD)/update.o: src/update.c src/apx.h src/net.h src/version.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc $(STB) -c -o $@ $<

$(BUILD)/main.o $(BUILD)/net.o: src/version.h

$(BUILD)/fileio.o: src/fileio.c src/apx.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Isrc $(STB) -c -o $@ $<

$(BUILD)/stb_impl.o: src/stb_impl.c src/fmath.h
	@mkdir -p $(BUILD)
	$(CC) $(LIBFLAGS) -Isrc $(STB) -c -o $@ $<

$(BUILD)/proto0: proto/proto0.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

# Dateien fuer den Amiga per HTTP bereitstellen (Amiga holt per wget)
serve:
	cd $(BUILD) && python3 -m http.server 8068

clean:
	rm -f $(BUILD)/*.o $(BUILD)/AmiPixelShop $(BUILD)/proto0

.PHONY: all serve clean
