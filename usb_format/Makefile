PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF := usb_format.elf

SRCS := src/main.c src/server.c src/i18n.c src/i18n_table.c src/mkfs.c src/hfsplus.c src/upcase.c src/geom_xml.c src/extfs.c src/fsdetect.c src/fsread.c \
        src/vollabel.c src/devtest.c src/library.c src/exfat_fsck.c src/smpapi.c src/sha256.c src/imgbak.c src/saves.c src/platform_ps5.c

# mkntfs (ntfs-3g) und mke2fs (e2fsprogs) für NTFS/ext4, gebaut mit tools/build_fstools.sh
TOOLS := tools/bin/mkntfs.elf tools/bin/mke2fs.elf

# fsck.exfat aus exfatprogs 1.4.3 (unverändert, GPL-2.0-or-later), mit src/exfat_shim.h übersetzt und im
# Formatierer aufgerufen (src/exfat_fsck.c); third_party/exfatprogs-ps5 ersetzt drei Linux-Header
EXFAT       := third_party/exfatprogs
EXFAT_SRCS  := $(EXFAT)/lib/libexfat.c $(EXFAT)/lib/exfat_fs.c $(EXFAT)/lib/exfat_dir.c $(EXFAT)/lib/utils.c \
               $(EXFAT)/fsck/fsck.c $(EXFAT)/fsck/repair.c
EXFAT_OBJS  := $(patsubst $(EXFAT)/%.c,build/exfatprogs/%.o,$(EXFAT_SRCS))
EXFAT_INC   := -I$(EXFAT)/include -I$(EXFAT)/fsck -Ithird_party/exfatprogs-ps5
EXFAT_FLAGS := -O2 -w -DNDEBUG -include src/exfat_shim.h $(EXFAT_INC)

# SystemService/UserService sind gelinkt (geladen), ihre Funktionen werden aber nur per Laufzeitsuche
# verwendet (platform_ps5.c: dynsym) – wie im PS5 Temperatur Studio.
# zstd 1.5.7 für die Image-Sicherung (.img.zst), ohne die strengen Warnungen des Projekts übersetzt
ZSTD_INC := -isystem third_party/zstd/lib
ZSTD_OBJ := build/zstd_all.o

LIBS := -lpthread -lkernel_sys -lSceSystemService -lSceUserService
HDRS := $(wildcard src/*.h)

# Version der Programme: Datei VERSION im Projekt (auch PC-Programme und App-Kachel), z. B. 2.5.3
APP_VERSION := $(shell cat ../VERSION)
CFLAGS := -Wall -Werror -O2 -DUSBFMT_VERSION='"$(APP_VERSION)"'

all: $(ELF)

# server.c bettet web/index.html und web/help.html, platform_ps5.c die Werkzeuge per .incbin ein -> aus dem
# Projektordner bauen. Vorher: Übersetzungen vollständig (Meldungen und Weboberfläche)?
$(ELF): $(SRCS) $(HDRS) web/index.html web/help.html web/bg.jpg web/page-dark.jpg web/page-light.jpg web/avatar.png $(TOOLS) \
        $(EXFAT_OBJS) $(ZSTD_OBJ) i18n/core.json i18n/web.json ../VERSION
	@command -v python3 >/dev/null || { echo "python3 fehlt - ohne es wären die Übersetzungen ungeprüft"; exit 1; }
	@echo "$(APP_VERSION)" | grep -Eqx "[0-9]+\.[0-9]+\.[0-9]+" || { echo "VERSION ungültig: $(APP_VERSION)"; exit 1; }
	python3 tools/i18n_gen.py && python3 tools/web_i18n.py --check
	$(CC) $(CFLAGS) $(EXFAT_INC) $(ZSTD_INC) -o $@ $(SRCS) $(EXFAT_OBJS) $(ZSTD_OBJ) $(LIBS)
	@# größer als 8 MB macht die ELF auf der PS5 Probleme
	@test $$(wc -c <$@) -le 8388608 || { echo "$@ ist größer als 8 MB"; rm -f $@; exit 1; }

# libexfat.c: die Linux-Geräteabfrage ersetzt exfat_fsck.c; fsck.c: main() heißt exfatprogs_fsck_main()
build/exfatprogs/%.o: $(EXFAT)/%.c src/exfat_shim.h
	@mkdir -p $(dir $@)
	$(CC) $(EXFAT_FLAGS) $(if $(findstring libexfat.c,$<),-Dexfat_get_blk_dev_info=exfatprogs_get_blk_dev_info_orig) \
	    $(if $(findstring fsck/fsck.c,$<),-Dmain=exfatprogs_fsck_main) -c $< -o $@

$(ZSTD_OBJ): src/zstd_all.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -w -DNDEBUG $(ZSTD_INC) -c $< -o $@

# Starter der App (Dump-Ordner, siehe dump/): nativer PS5-Titel mit eingebetteter ELF,
# umgewandelt und signiert mit ps5-native-tool (siehe app/build_title.sh)
app: app/eboot.bin

app/eboot.bin: app/title/starter.c app/title/title.ld app/title/symbols.map app/build_title.sh $(ELF)
	bash app/build_title.sh

clean:
	rm -rf $(ELF) app/eboot.bin app/build build

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

.PHONY: all app clean test
