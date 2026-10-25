# --- Compilation Variables ---
CC = gcc
CFLAGS = -Wall -Wextra -O2 $(shell pkg-config --cflags freetype2 2>/dev/null) $(shell pkg-config --cflags libpng 2>/dev/null)
# Detect packages independently: a missing FreeType .pc file must not hide the available libpng linker flags.
LIBS = -lz -lm -lsqlite3 $(shell pkg-config --libs freetype2 2>/dev/null) $(shell pkg-config --libs libpng 2>/dev/null || echo "-lpng16")
# WebP/AVIF декодеры необязательны: без пакета остаётся поддержка stb_image.
# Не подключаем pkg-config библиотеки Linux при сборке Win32/MinGW.
ifneq ($(OS),Windows_NT)
ifneq ($(CROSS),win32)
    HAS_WEBP := $(shell pkg-config --exists libwebp && echo yes)
    ifeq ($(HAS_WEBP),yes)
        CFLAGS += -DHAVE_WEBP $(shell pkg-config --cflags libwebp)
        LIBS += $(shell pkg-config --libs libwebp)
    endif
    HAS_AVIF := $(shell pkg-config --exists libavif && echo yes)
    ifeq ($(HAS_AVIF),yes)
        CFLAGS += -DHAVE_AVIF $(shell pkg-config --cflags libavif)
        LIBS += $(shell pkg-config --libs libavif)
    endif
endif
endif
STRIP = strip

# --- Output Directory ---
OBJDIR = build
COMPILE_DIR = compile
VERSION = 1.0.0

# --- Platform Detection ---
ifeq ($(OS),Windows_NT)
    PLATFORM = win32
    TARGET = my_browser.exe
    LIBS += -lgdi32 -luser32 -lws2_32 -lwinhttp
    PACKAGE_DIR = $(COMPILE_DIR)/windows
    ARCHIVE = mlbrowser-$(VERSION)-win64.zip
else ifeq ($(CROSS),win32)
    CC = x86_64-w64-mingw32-gcc
    STRIP = x86_64-w64-mingw32-strip
    PLATFORM = win32
    TARGET = my_browser.exe
    LIBS += -lgdi32 -luser32 -lws2_32 -lwinhttp -lm
    HAS_ZLIB = $(shell echo "int main(){}" | x86_64-w64-mingw32-gcc -x c - -lz -o /dev/null 2>/dev/null && echo "yes")
    ifeq ($(HAS_ZLIB),yes)
        LIBS += -lz
    endif
    HAS_SQLITE = $(shell echo "int main(){}" | x86_64-w64-mingw32-gcc -x c - -lsqlite3 -o /dev/null 2>/dev/null && echo "yes")
    ifeq ($(HAS_SQLITE),yes)
        LIBS += -lsqlite3
    endif
    PACKAGE_DIR = $(COMPILE_DIR)/windows
    ARCHIVE = mlbrowser-$(VERSION)-win64.zip
else ifeq ($(PLATFORM),wayland)
    CFLAGS += $(shell pkg-config --cflags wayland-client xkbcommon)
    LIBS += $(shell pkg-config --libs wayland-client xkbcommon) -ldl
    TARGET = my_browser
    PACKAGE_DIR = $(COMPILE_DIR)/linux
    ARCHIVE = mlbrowser-$(VERSION)-linux-wayland.tar.gz
else
    PLATFORM = x11
    LIBS += -lX11 -ldl
    TARGET = my_browser
    PACKAGE_DIR = $(COMPILE_DIR)/linux
    ARCHIVE = mlbrowser-$(VERSION)-linux-x11.tar.gz
endif

# --- Module System Implementation ---
MOD_SRCS :=
MOD_CFLAGS :=

# Discover all modules
ALL_MOD_DIRS := $(wildcard mods/frame/*) $(wildcard mods/render/*) mods/gal/common mods/nal/common
ifeq ($(PLATFORM),win32)
    ALL_MOD_DIRS += mods/gal/win32 mods/nal/win32
else ifeq ($(PLATFORM),wayland)
    ALL_MOD_DIRS += mods/gal/wayland mods/nal/linux
else
    ALL_MOD_DIRS += mods/gal/x11 mods/nal/linux
endif

# Filter out disabled modules
ifdef DISABLED_MODS
    $(foreach mod,$(DISABLED_MODS),$(eval ALL_MOD_DIRS := $(filter-out %/$(mod),$(ALL_MOD_DIRS))))
endif

# Include each module's makefile
$(foreach dir,$(ALL_MOD_DIRS),$(eval MOD_PATH := $(dir))$(eval include $(dir)/module.mk))

# Apply module settings
SRCS = main.c $(MOD_SRCS)
CFLAGS += $(foreach dir,$(ALL_MOD_DIRS),-I$(dir))
CFLAGS += $(MOD_CFLAGS)

# Feature flags based on folder presence
ifneq ($(filter mods/render/%,$(ALL_MOD_DIRS)),)
    CFLAGS += -DHAVE_RENDER
endif
ifneq ($(filter mods/frame/ui%,$(ALL_MOD_DIRS)),)
    CFLAGS += -DHAVE_UI
endif
ifneq ($(filter mods/frame/logger%,$(ALL_MOD_DIRS)),)
    CFLAGS += -DHAVE_LOGGER
endif
ifneq ($(filter mods/frame/net%,$(ALL_MOD_DIRS)),)
    CFLAGS += -DHAVE_NET
endif

# --- Targets ---
all: $(TARGET)

# Инкрементальная сборка: объектные файлы + автоматические зависимости от заголовков
OBJS = $(patsubst %.c,$(OBJDIR)/%.o,$(SRCS))

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET) $(LIBS)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(OBJS:.o=.d)

# Packaging
package: $(TARGET)
	mkdir -p $(PACKAGE_DIR)/assets
	mkdir -p $(PACKAGE_DIR)/cache
	cp $(TARGET) $(PACKAGE_DIR)/
	cp -r assets/* $(PACKAGE_DIR)/assets/
	$(STRIP) $(PACKAGE_DIR)/$(TARGET)
ifeq ($(PLATFORM),win32)
	# Assuming zip tool is available for Windows packaging
	cd $(PACKAGE_DIR) && zip -r ../$(ARCHIVE) .
else
	cd $(PACKAGE_DIR) && tar -czvf ../$(ARCHIVE) .
endif

# Regression tests for native image decoding + responsive source selection.
TEST_OBJS = $(filter-out $(OBJDIR)/main.o,$(OBJS))
test-images: $(TEST_OBJS) tests/images/check.c
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -DTEST_WEBP_PATH=\"$(CURDIR)/tests/images/red.webp\" -DTEST_AVIF_PATH=\"$(CURDIR)/tests/images/blue.avif\" tests/images/check.c $(TEST_OBJS) -o $(OBJDIR)/images_test $(LIBS)
	./$(OBJDIR)/images_test

clean:
	rm -rf $(TARGET) $(COMPILE_DIR) $(OBJDIR) browser.log
	rm -f my_browser my_browser.exe

.PHONY: all clean package test-images
