# --- Compilation Variables ---
CC = gcc
CFLAGS = -Wall -Wextra -O2 $(shell pkg-config --cflags freetype2 2>/dev/null) $(shell pkg-config --cflags libpng 2>/dev/null)
# Detect packages independently: a missing FreeType .pc file must not hide the available libpng linker flags.
LIBS = -lz -lm -lsqlite3 $(shell pkg-config --libs freetype2 2>/dev/null) $(shell pkg-config --libs libpng 2>/dev/null || echo "-lpng16")
# Optional Unicode bidi algorithm (ICU); HarfBuzz is loaded dynamically at runtime.
ifneq ($(OS),Windows_NT)
ifneq ($(CROSS),win32)
    HAS_ICU := $(shell pkg-config --exists icu-uc && echo yes)
    ifeq ($(HAS_ICU),yes)
        CFLAGS += -DTTF_HAVE_ICU $(shell pkg-config --cflags icu-uc)
        LIBS += $(shell pkg-config --libs icu-uc) -ldl
    endif
endif
endif
# Optional AES-GCM password vault encryption (never stores credentials in plaintext).
ifneq ($(OS),Windows_NT)
ifneq ($(CROSS),win32)
    HAS_OPENSSL := $(shell pkg-config --exists openssl && echo yes)
    ifeq ($(HAS_OPENSSL),yes)
        CFLAGS += -DHAVE_OPENSSL_VAULT $(shell pkg-config --cflags openssl)
        LIBS += $(shell pkg-config --libs openssl)
    endif
endif
endif
# WebRTC is being implemented in-tree (no libdatachannel or other WebRTC stack dependency).
# WebP/AVIF декодеры необязательны: без пакета остаётся поддержка stb_image.
# Optional libcurl transport: HTTP/2/HTTP/3 (when compiled in), Brotli decoding and proxy support.
# Win32/MinGW builds keep using their existing backend and do not link host libraries.
ifneq ($(OS),Windows_NT)
ifneq ($(CROSS),win32)
    HAS_CURL := $(shell pkg-config --exists libcurl && echo yes)
    ifeq ($(HAS_CURL),yes)
        CFLAGS += -DMLBROWSER_HAVE_CURL $(shell pkg-config --cflags libcurl)
        LIBS += $(shell pkg-config --libs libcurl)
    endif
endif
endif
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
    LIBS += -lgdi32 -luser32 -lws2_32 -lwinhttp -lcomdlg32 -lshell32 -lole32 -limm32
    PACKAGE_DIR = $(COMPILE_DIR)/windows
    ARCHIVE = mlbrowser-$(VERSION)-win64.zip
else ifeq ($(CROSS),win32)
    CC = x86_64-w64-mingw32-gcc
    STRIP = x86_64-w64-mingw32-strip
    PLATFORM = win32
    TARGET = my_browser.exe
    LIBS += -lgdi32 -luser32 -lws2_32 -lwinhttp -lcomdlg32 -lshell32 -lole32 -limm32 -lm
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

# Keep the embedded JS helper synchronized with its source before compiling wasm.c.
mods/render/webpage/wasm_node_host.h: mods/render/webpage/wasm_node_host.js tools/embed_wasm_host.py
	python3 tools/embed_wasm_host.py

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(OBJS:.o=.d)

# QuickJS is a very large single translation unit; use a configurable, faster
# optimization level for it so clean builds finish on constrained CI/dev hosts.
QUICKJS_OPT_LEVEL ?= -O1
$(OBJDIR)/mods/frame/quickjs/quickjs.o: CFLAGS += $(QUICKJS_OPT_LEVEL)
# A few legacy translation units are unusually expensive for GCC -O2.  -O1
# keeps clean builds reliable without de-optimizing the rest of the browser.
$(OBJDIR)/mods/render/webpage/css.o \
$(OBJDIR)/mods/render/webpage/style.o \
$(OBJDIR)/mods/render/webpage/layout2.o \
$(OBJDIR)/mods/render/webpage/images.o \
$(OBJDIR)/mods/render/webpage/svg.o \
$(OBJDIR)/mods/render/webpage/paint.o: CFLAGS += -O1

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

# EGL/GLES2 test is intentionally compiled without GL development headers or link-time EGL deps.
test-webgl:
	@mkdir -p $(OBJDIR)
	$(CC) -Wall -Wextra -O2 -Imods/render/webpage tests/webgl/check.c mods/render/webpage/webgl_backend.c -ldl -o $(OBJDIR)/webgl_test
	@./$(OBJDIR)/webgl_test; rc=$$?; if [ $$rc -eq 77 ]; then echo "SKIP: EGL/GLES2 runtime unavailable"; exit 0; else exit $$rc; fi

test-media:
	@command -v ffmpeg >/dev/null && command -v ffprobe >/dev/null && command -v ffplay >/dev/null || { echo "SKIP: FFmpeg/ffprobe/ffplay not installed"; exit 0; }
	@mkdir -p $(OBJDIR) tests/media
	$(CC) -Wall -Wextra -O2 -Imods/render/webpage tests/media/check.c mods/render/webpage/media_backend.c mods/render/webpage/media_byte_queue.c -lm -o $(OBJDIR)/media_test
	$(CC) -Wall -Wextra -O2 -Imods/render/webpage tests/media/check_audio.c mods/render/webpage/media_backend.c mods/render/webpage/media_byte_queue.c -lm -o $(OBJDIR)/media_audio_test
	ffmpeg -y -hide_banner -loglevel error -f lavfi -i color=c=red:s=32x32:r=8:d=1 -an -c:v mpeg4 -pix_fmt yuv420p tests/media/test-video.mp4
	ffmpeg -y -hide_banner -loglevel error -f lavfi -i sine=frequency=440:duration=0.6 -c:a pcm_s16le tests/media/test-audio.wav
	./$(OBJDIR)/media_test tests/media/test-video.mp4; rc=$$?; rm -f tests/media/test-video.mp4; if [ $$rc -ne 0 ]; then rm -f tests/media/test-audio.wav; exit $$rc; fi
	SDL_AUDIODRIVER=dummy ./$(OBJDIR)/media_audio_test tests/media/test-audio.wav; rc=$$?; rm -f tests/media/test-audio.wav; exit $$rc

.PHONY: test-webgl test-media

# Verify that HTMLMediaElement playbackRate scales video PTS deadlines instead of
# merely scaling the JavaScript currentTime clock.
test-media-rate:
	@command -v ffmpeg >/dev/null && command -v ffprobe >/dev/null && command -v ffplay >/dev/null || { echo "SKIP: FFmpeg tools not installed"; exit 0; }
	@mkdir -p $(OBJDIR) tests/media
	$(CC) -Wall -Wextra -O2 -Imods/render/webpage tests/media/check_rate.c mods/render/webpage/media_backend.c mods/render/webpage/media_byte_queue.c -lm -pthread -o $(OBJDIR)/media_rate_test
	ffmpeg -y -hide_banner -loglevel error -f lavfi -i color=c=red:s=32x32:r=8:d=1 -an -c:v mpeg4 -g 8 -pix_fmt yuv420p tests/media/test-rate.mp4
	./$(OBJDIR)/media_rate_test tests/media/test-rate.mp4; rc=$$?; rm -f tests/media/test-rate.mp4; exit $$rc

.PHONY: test-media-rate

test-media-audio-rate:
	@command -v ffmpeg >/dev/null && command -v ffprobe >/dev/null && command -v ffplay >/dev/null || { echo "SKIP: FFmpeg tools not installed"; exit 0; }
	@mkdir -p $(OBJDIR) tests/media
	$(CC) -Wall -Wextra -O2 -Imods/render/webpage tests/media/check_audio_rate.c mods/render/webpage/media_backend.c mods/render/webpage/media_byte_queue.c -lm -pthread -o $(OBJDIR)/media_audio_rate_test
	ffmpeg -y -hide_banner -loglevel error -f lavfi -i sine=frequency=440:duration=1.6 -c:a pcm_s16le tests/media/test-audio-rate.wav
	SDL_AUDIODRIVER=dummy ./$(OBJDIR)/media_audio_rate_test tests/media/test-audio-rate.wav; rc=$$?; rm -f tests/media/test-audio-rate.wav; exit $$rc

.PHONY: test-media-audio-rate

# Verify that HTML media audio is decoded to bounded PCM and the SDL playback
# queue supplies the master clock used to schedule video PTS.
test-media-av-clock:
	@command -v ffmpeg >/dev/null && command -v ffprobe >/dev/null || { echo "SKIP: FFmpeg tools not installed"; exit 0; }
	@mkdir -p $(OBJDIR) tests/media
	$(CC) -Wall -Wextra -O2 -Imods/render/webpage tests/media/av_clock_check.c mods/render/webpage/media_backend.c mods/render/webpage/media_byte_queue.c -lm -ldl -o $(OBJDIR)/media_av_clock_test
	ffmpeg -y -hide_banner -loglevel error -f lavfi -i color=c=red:s=32x32:r=8:d=1.5 -f lavfi -i sine=frequency=440:duration=1.5 -shortest -c:v mpeg4 -g 8 -pix_fmt yuv420p -c:a aac -b:a 64k -movflags +faststart tests/media/test-av-clock.mp4
	SDL_AUDIODRIVER=dummy ./$(OBJDIR)/media_av_clock_test tests/media/test-av-clock.mp4; rc=$$?; rm -f tests/media/test-av-clock.mp4; exit $$rc

.PHONY: test-media-av-clock

# Exercise callback-clock underrun accounting by stalling the caller while the
# SDL dummy device drains a bounded PCM ring.
test-media-audio-underrun:
	@command -v ffmpeg >/dev/null && command -v ffprobe >/dev/null || { echo "SKIP: FFmpeg tools not installed"; exit 0; }
	@mkdir -p $(OBJDIR) tests/media
	$(CC) -Wall -Wextra -O2 -Imods/render/webpage tests/media/audio_underrun_check.c mods/render/webpage/media_backend.c mods/render/webpage/media_byte_queue.c -lm -ldl -o $(OBJDIR)/media_audio_underrun_test
	ffmpeg -y -hide_banner -loglevel error -f lavfi -i sine=frequency=440:duration=2.2 -c:a pcm_s16le tests/media/test-audio-underrun.wav
	SDL_AUDIODRIVER=dummy ./$(OBJDIR)/media_audio_underrun_test tests/media/test-audio-underrun.wav; rc=$$?; rm -f tests/media/test-audio-underrun.wav; exit $$rc

.PHONY: test-media-audio-underrun

# Exercise runtime gain/mute on the SDL callback path; changing volume must not
# restart FFmpeg or reset the consumed-sample media clock.
test-media-live-volume:
	@command -v ffmpeg >/dev/null && command -v ffprobe >/dev/null || { echo "SKIP: FFmpeg tools not installed"; exit 0; }
	@mkdir -p $(OBJDIR) tests/media
	$(CC) -Wall -Wextra -O2 -Imods/render/webpage tests/media/audio_live_volume_check.c mods/render/webpage/media_backend.c mods/render/webpage/media_byte_queue.c -lm -ldl -o $(OBJDIR)/media_live_volume_test
	ffmpeg -y -hide_banner -loglevel error -f lavfi -i sine=frequency=440:duration=1.8 -c:a pcm_s16le tests/media/test-live-volume.wav
	SDL_AUDIODRIVER=dummy ./$(OBJDIR)/media_live_volume_test tests/media/test-live-volume.wav; rc=$$?; rm -f tests/media/test-live-volume.wav; exit $$rc

.PHONY: test-media-live-volume

# Validate HTMLMediaElement volume/muted JS bridge against the live SDL gain path.
test-media-live-volume-browser: $(TARGET)
	bash tests/media_live_volume.sh

.PHONY: test-media-live-volume-browser

# Ensure internal seek/rate restarts preserve ready state and do not duplicate
# play/playing events; an explicit pause/resume must start a new lifecycle.
test-media-restart-events: $(TARGET)
	bash tests/media_restart_events.sh

.PHONY: test-media-restart-events

# Verify pause/resume freezes the same backend clock and resumes without replaying old PCM.
test-media-pause-resume: $(TARGET)
	@command -v ffmpeg >/dev/null && command -v ffprobe >/dev/null || { echo "SKIP: FFmpeg/ffprobe not installed"; exit 0; }
	@mkdir -p $(OBJDIR) tests/media
	$(CC) -Wall -Wextra -O2 -Imods/render/webpage tests/media/pause_resume_check.c mods/render/webpage/media_backend.c mods/render/webpage/media_byte_queue.c -lm -ldl -o $(OBJDIR)/media_pause_resume_test
	ffmpeg -y -hide_banner -loglevel error -f lavfi -i color=c=red:s=64x48:r=10:d=3.6 -f lavfi -i sine=frequency=440:duration=3.6 -shortest -c:v mpeg4 -g 10 -pix_fmt yuv420p -c:a aac -b:a 64k -movflags +faststart tests/media/test-pause-resume-av.mp4
	ffmpeg -y -hide_banner -loglevel error -f lavfi -i color=c=blue:s=64x48:r=10:d=3.6 -an -c:v mpeg4 -g 10 -pix_fmt yuv420p tests/media/test-pause-resume-video.mp4
	SDL_AUDIODRIVER=dummy ./$(OBJDIR)/media_pause_resume_test tests/media/test-pause-resume-av.mp4 tests/media/test-pause-resume-video.mp4; rc=$$?; rm -f tests/media/test-pause-resume-av.mp4 tests/media/test-pause-resume-video.mp4; exit $$rc

.PHONY: test-media-pause-resume

test-canvas-gradients:
	@if command -v node >/dev/null 2>&1; then node tests/canvas/check_gradients.js; else echo "SKIP: node not installed"; fi

.PHONY: test-canvas-gradients

# UI persistence helpers and basic PDF/source viewer regression tests.
test-browser-ui: $(TEST_OBJS) tests/ui_features/check_db.c tests/ui_features/check_helpers.c main.c
	$(CC) $(CFLAGS) -I. tests/ui_features/check_helpers.c $(TEST_OBJS) -o $(OBJDIR)/browser_helpers_test $(LIBS)
	$(CC) $(CFLAGS) -I mods/frame/db tests/ui_features/check_db.c $(TEST_OBJS) -o $(OBJDIR)/browser_db_test $(LIBS)
	@tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; mkdir -p "$$tmp/home"; \
	  (cd "$$tmp" && HOME="$$tmp/home" "$(CURDIR)/$(OBJDIR)/browser_helpers_test") && \
	  (cd "$$tmp" && HOME="$$tmp/home" "$(CURDIR)/$(OBJDIR)/browser_db_test") && python3 tests/ui_features/print_pipeline_test.py

.PHONY: test-browser-ui

# Background download controller, SQLite resume metadata and real local HTTP range integration.
test-downloads:
	python3 tests/downloads/controller_contract_test.py
	python3 tests/downloads/schema_resume_test.py
	python3 tests/downloads/transfer_integration_server.py

.PHONY: test-downloads

# CSS feature parser regression coverage for the compact browser engine.
test-css-features: $(TEST_OBJS) tests/css/check_features.c
	$(CC) $(CFLAGS) -I mods/render/webpage tests/css/check_features.c $(TEST_OBJS) -o $(OBJDIR)/css_features_test $(LIBS)
	./$(OBJDIR)/css_features_test

# Isolated tree-construction tests: no UI, network, CSS cascade or image decoding.
test-html5-tree-builder: tests/html5_tree_builder_check.c mods/render/webpage/html.c mods/render/webpage/url.c mods/render/webpage/webpage.h
	@mkdir -p $(OBJDIR)
	$(CC) -Wall -Wextra -O2 -ffunction-sections -fdata-sections -I mods/render/webpage -I mods/render/parser -I mods/nal/common -I mods/gal/common tests/html5_tree_builder_check.c mods/render/webpage/html.c mods/render/webpage/url.c -Wl,--gc-sections -o $(OBJDIR)/html5_tree_builder_test
	./$(OBJDIR)/html5_tree_builder_test
	python3 tools/run_html5lib_tree_tests.py tests/html5lib_subset/tree-construction --parser $(OBJDIR)/html5_tree_builder_test

# Optional external differential run. The html5lib-tests checkout is supplied by the caller.
HTML5LIB_TESTS ?=
test-html5lib-tree-builder: test-html5-tree-builder
	@test -n "$(HTML5LIB_TESTS)" || { echo "Set HTML5LIB_TESTS=/path/to/html5lib-tests/tree-construction"; exit 2; }
	python3 tools/run_html5lib_tree_tests.py "$(HTML5LIB_TESTS)" --parser $(OBJDIR)/html5_tree_builder_test

.PHONY: test-html5-tree-builder test-html5lib-tree-builder test-css-features


# Native structural WebAssembly preflight, plus real runtime regressions in MLBrowser.
test-wasm-binary: mods/render/webpage/wasm_binary.c mods/render/webpage/wasm_binary.h tests/wasm_binary_check.c
	@mkdir -p $(OBJDIR)
	$(CC) -Wall -Wextra -Werror -O2 -Imods/render/webpage tests/wasm_binary_check.c mods/render/webpage/wasm_binary.c -o $(OBJDIR)/wasm_binary_test
	./$(OBJDIR)/wasm_binary_test

# In-process native C interpreter with stack-type validation for its supported native instruction subset.
test-wasm-runtime: mods/render/webpage/wasm_runtime.c mods/render/webpage/wasm_runtime.h tests/wasm_runtime_check.c tests/wasm/probe.wasm tests/wasm/grow_zero.wasm tests/wasm/import_func.wasm tests/wasm/control_flow.wasm tests/wasm/indirect_call.wasm tests/wasm/float_ops.wasm tests/wasm/bulk_memory.wasm tests/wasm/resource_exports.wasm tests/wasm/import_memory_global.wasm tests/wasm/import_table_caller.wasm tests/wasm/import_table_callee.wasm tests/wasm/trunc_trap.wasm tests/wasm/rem_overflow.wasm tests/wasm/sign_extend.wasm tests/wasm/reference_table_ops.wasm tests/wasm/reference_elem_expr.wasm tests/wasm/reference_bad_func.wasm tests/wasm/externref_table_global.wasm tests/wasm/externref_table_global.wat tests/wasm/make_externref_fixture.py tests/wasm/multi_value.wasm tests/wasm/multi_value.wat tests/wasm/multi_value_import.wasm tests/wasm/multi_value_import.wat tests/wasm/make_multivalue_fixture.py tests/wasm/blocktype_multivalue.wasm tests/wasm/make_blocktype_fixture.py tests/wasm/make_validation_corpus.py
	python3 tests/wasm/make_validation_corpus.py
	@mkdir -p $(OBJDIR)
	$(CC) -Wall -Wextra -Werror -O2 -Imods/render/webpage tests/wasm_runtime_check.c mods/render/webpage/wasm_runtime.c -lm -o $(OBJDIR)/wasm_runtime_test
	./$(OBJDIR)/wasm_runtime_test .

# Differential WebAssembly checks: compare validation and execution with Node/V8.
# Generated fixtures stay inside the native runtime instruction subset.
test-wasm-differential: test-wasm-runtime tests/wasm_validate_cli.c tests/wasm_run_cli.c tests/wasm/differential_validate.js tests/wasm/differential_execute.js
	$(CC) -Wall -Wextra -Werror -O2 -Imods/render/webpage tests/wasm_validate_cli.c mods/render/webpage/wasm_runtime.c -lm -o $(OBJDIR)/wasm_validate_cli
	node tests/wasm/differential_validate.js ./$(OBJDIR)/wasm_validate_cli tests/wasm
	$(CC) -Wall -Wextra -Werror -O2 -Imods/render/webpage tests/wasm_run_cli.c mods/render/webpage/wasm_runtime.c -lm -o $(OBJDIR)/wasm_run_cli
	node tests/wasm/differential_execute.js ./$(OBJDIR)/wasm_run_cli tests/wasm

.PHONY: test-wasm-differential


# Adapter self-test is part of the normal suite; the upstream checkout and WABT
# are intentionally external so the normal build stays offline/reproducible.
WASM_SPEC_SUITE ?=
WASM_SPEC_FILES ?= block.wast,br.wast,br_if.wast,br_table.wast,if.wast,labels.wast,return.wast
WAST2JSON ?= wast2json

test-wasm-spec-adapter: test-wasm-differential tests/wasm/run_official_core.py tests/wasm/validate_spec_batch.js
	python3 tests/wasm/run_official_core.py --self-test --validator ./$(OBJDIR)/wasm_validate_cli --node "$$(command -v node)"

.PHONY: test-wasm-spec-adapter

test-wasm-spec: test-wasm-differential tests/wasm/run_official_core.py tests/wasm/validate_spec_batch.js
	@test -n "$(WASM_SPEC_SUITE)" || { echo "Set WASM_SPEC_SUITE to a WebAssembly/testsuite checkout (or WebAssembly/spec checkout)."; exit 2; }
	python3 tests/wasm/run_official_core.py --suite "$(WASM_SPEC_SUITE)" --files "$(WASM_SPEC_FILES)" --wast2json "$(WAST2JSON)" --validator ./$(OBJDIR)/wasm_validate_cli --node "$$(command -v node)"

$(OBJDIR)/wasm_script_cli: tests/wasm_script_cli.c mods/render/webpage/wasm_runtime.c mods/render/webpage/wasm_runtime.h
	@mkdir -p $(OBJDIR)
	$(CC) -Wall -Wextra -Werror -O2 -Imods/render/webpage tests/wasm_script_cli.c mods/render/webpage/wasm_runtime.c -lm -o $@

test-wasm-spec-runtime-adapter: $(OBJDIR)/wasm_script_cli tests/wasm/run_official_runtime.py tests/wasm/execute_official_script.js
	python3 tests/wasm/run_official_runtime.py --self-test --native-runner ./$(OBJDIR)/wasm_script_cli --node "$$(command -v node)"

.PHONY: test-wasm-spec-runtime-adapter

test-wasm-spec-runtime: test-wasm-spec test-wasm-spec-runtime-adapter
	python3 tests/wasm/run_official_runtime.py --suite "$(WASM_SPEC_SUITE)" --files "$(WASM_SPEC_FILES)" --wast2json "$(WAST2JSON)" --native-runner ./$(OBJDIR)/wasm_script_cli --node "$$(command -v node)"

.PHONY: test-wasm-spec test-wasm-spec-runtime

# Optional libFuzzer harness for parser/preflight hardening. Requires Clang + libFuzzer.
FUZZ_CC ?= clang
fuzz-wasm:
	@command -v $(FUZZ_CC) >/dev/null 2>&1 || { echo "Clang not found; install clang to build the WASM fuzzer"; exit 1; }
	@mkdir -p $(OBJDIR)
	$(FUZZ_CC) -Wall -Wextra -O1 -g -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined \
		-Imods/render/webpage tests/wasm_fuzz.c mods/render/webpage/wasm_binary.c \
		mods/render/webpage/wasm_runtime.c -lm -o $(OBJDIR)/wasm_fuzzer
	@echo "Built $(OBJDIR)/wasm_fuzzer; run 'make fuzz-wasm-run' to fuzz the bundled WASM fixtures."

fuzz-wasm-run: fuzz-wasm
	@set -eu; corpus=$$(mktemp -d); trap 'rm -rf "$$corpus"' EXIT; \
		cp tests/wasm/*.wasm tests/wasm/*.wat tests/wasm/*.c "$$corpus"/; \
		./$(OBJDIR)/wasm_fuzzer -verbosity=0 -max_total_time=30 -max_len=65536 "$$corpus"

.PHONY: fuzz-wasm fuzz-wasm-run

# Native runtime handles supported modules; Node/V8 remains a fallback for unsupported features/imports.
test-wasm-native: $(TARGET)
	bash tests/wasm_native.sh

.PHONY: test-wasm-native

test-wasm: $(TARGET) test-wasm-binary test-wasm-runtime test-wasm-differential test-wasm-spec-adapter test-wasm-spec-runtime-adapter test-wasm-native
	@command -v node >/dev/null 2>&1 || { echo "FAIL: Node.js is required for WebAssembly execution"; exit 1; }
	node --check mods/render/webpage/wasm_node_host.js
	bash tests/dom_js.sh
	bash tests/wasm_timeout.sh

.PHONY: test-wasm test-wasm-binary test-wasm-runtime

# Bound string-copy and concatenation regressions for the compact JS engine.
test-js-engine-strings: $(OBJDIR)/mods/frame/js_engine/start.o tests/js_engine/check_strings.c
	$(CC) $(CFLAGS) -I mods/frame/js_engine tests/js_engine/check_strings.c $(OBJDIR)/mods/frame/js_engine/start.o -o $(OBJDIR)/js_engine_strings_test -lm
	./$(OBJDIR)/js_engine_strings_test

.PHONY: test-js-engine-strings

# AddressSanitizer regression for the bounded external-SVG sprite cache signature.
test-svg-cache-bounds:
	python3 tests/svg/check_cache_bounds.py

.PHONY: test-svg-cache-bounds

# Clang libFuzzer harnesses for untrusted HTML/CSS/SVG/URL inputs. These are
# opt-in so normal release builds do not depend on Clang or sanitizer runtimes.
FUZZ_CC ?= clang
FUZZ_FLAGS ?= -O1 -g -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined
FUZZDIR = $(OBJDIR)/fuzz

$(FUZZDIR)/html_fuzzer: tests/html_fuzz.c mods/render/parser/start.c mods/render/parser/parser.h
	@mkdir -p $(FUZZDIR)
	$(FUZZ_CC) $(FUZZ_FLAGS) -I mods/render/parser -I mods/gal/common tests/html_fuzz.c mods/render/parser/start.c -o $@

# Fuzz the primary HTML tree builder, including URL resolution and resource metadata,
# while stubbing network/style/image integrations to keep inputs deterministic.
$(FUZZDIR)/html_build_fuzzer: tests/html_build_fuzz.c mods/render/webpage/html.c mods/render/webpage/url.c mods/render/webpage/webpage.h
	@mkdir -p $(FUZZDIR)
	$(FUZZ_CC) $(FUZZ_FLAGS) -ffunction-sections -fdata-sections -I mods/render/webpage -I mods/render/parser -I mods/nal/common -I mods/gal/common tests/html_build_fuzz.c mods/render/webpage/html.c mods/render/webpage/url.c -Wl,--gc-sections -o $@

$(FUZZDIR)/css_fuzzer: tests/css_fuzz.c mods/render/webpage/css.c mods/render/webpage/css_int.h
	@mkdir -p $(FUZZDIR)
	$(FUZZ_CC) $(FUZZ_FLAGS) -I mods/render/webpage -I mods/render/parser -I mods/nal/common -I mods/gal/common -I mods/render/font_manager -I mods/render/ttf_render tests/css_fuzz.c mods/render/webpage/css.c -lm -o $@

$(FUZZDIR)/svg_fuzzer: tests/svg_fuzz.c mods/render/webpage/svg.c mods/render/webpage/svg.h
	@mkdir -p $(FUZZDIR)
	$(FUZZ_CC) $(FUZZ_FLAGS) -I mods/render/webpage tests/svg_fuzz.c mods/render/webpage/svg.c -lm -o $@

$(FUZZDIR)/url_fuzzer: tests/url_fuzz.c mods/render/webpage/url.c mods/render/webpage/webpage.h
	@mkdir -p $(FUZZDIR)
	$(FUZZ_CC) $(FUZZ_FLAGS) -I mods/render/webpage -I mods/render/parser tests/url_fuzz.c mods/render/webpage/url.c -o $@

fuzz-parsers: $(FUZZDIR)/html_fuzzer $(FUZZDIR)/html_build_fuzzer $(FUZZDIR)/css_fuzzer $(FUZZDIR)/svg_fuzzer $(FUZZDIR)/url_fuzzer
	@echo "Built ASan/UBSan libFuzzer targets: legacy HTML, primary HTML tree builder, CSS, SVG and URL."

# Each fuzzer gets an isolated temporary corpus, preserving checked-in seed files.
# Override FUZZ_SECONDS to increase the local exploration time.
FUZZ_SECONDS ?= 5
fuzz-parsers-run: fuzz-parsers
	@set -eu; tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; \
	  for kind in html css svg url; do mkdir -p "$$tmp/$$kind"; cp tests/fuzz_corpus/$$kind/* "$$tmp/$$kind/"; done; \
	  echo "Fuzzing HTML parser (ASan+UBSan, $(FUZZ_SECONDS)s)"; \
	  ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 ./$(FUZZDIR)/html_fuzzer -verbosity=0 -print_final_stats=1 -max_total_time=$(FUZZ_SECONDS) -max_len=262144 "$$tmp/html"; \
	  echo "PASS: HTML parser fuzz run"; \
	  mkdir -p "$$tmp/html_build" "$$tmp/html_build_work"; cp tests/fuzz_corpus/html/* "$$tmp/html_build/"; \
	  echo "Fuzzing primary HTML tree builder (ASan+UBSan, $(FUZZ_SECONDS)s)"; \
	  (cd "$$tmp/html_build_work" && ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 "$(abspath $(FUZZDIR)/html_build_fuzzer)" -verbosity=0 -print_final_stats=1 -rss_limit_mb=1024 -max_total_time=$(FUZZ_SECONDS) -max_len=262144 "$$tmp/html_build"); \
	  echo "PASS: primary HTML tree builder fuzz run"; \
	  echo "Fuzzing CSS parser (ASan+UBSan, $(FUZZ_SECONDS)s)"; \
	  ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 ./$(FUZZDIR)/css_fuzzer -verbosity=0 -print_final_stats=1 -max_total_time=$(FUZZ_SECONDS) -max_len=131072 "$$tmp/css"; \
	  echo "PASS: CSS parser fuzz run"; \
	  echo "Fuzzing SVG parser (ASan+UBSan, $(FUZZ_SECONDS)s)"; \
	  ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 ./$(FUZZDIR)/svg_fuzzer -verbosity=0 -print_final_stats=1 -max_total_time=$(FUZZ_SECONDS) -max_len=1048576 "$$tmp/svg"; \
	  echo "PASS: SVG parser fuzz run"; \
	  echo "Fuzzing URL resolver (ASan+UBSan, $(FUZZ_SECONDS)s)"; \
	  ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 ./$(FUZZDIR)/url_fuzzer -verbosity=0 -print_final_stats=1 -max_total_time=$(FUZZ_SECONDS) -max_len=32768 "$$tmp/url"; \
	  echo "PASS: URL resolver fuzz run"

# Boundary test compiles the production URL resolver by itself with both sanitizers.
test-url-resolver:
	@mkdir -p $(FUZZDIR)
	$(FUZZ_CC) -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -ffunction-sections -fdata-sections -I mods/render/webpage -I mods/render/parser tests/net/url_resolve_test.c mods/render/webpage/url.c -o $(FUZZDIR)/url_resolve_test
	ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 ./$(FUZZDIR)/url_resolve_test

.PHONY: fuzz-parsers fuzz-parsers-run test-url-resolver

# Optional HarfBuzz+ICU shaping smoke test (requires ICU development headers/libs).
test-text-shaping:
	@if pkg-config --exists icu-uc; then \
	  FONT=$$(find /usr/share/fonts -type f -iname 'DejaVuSans.ttf' 2>/dev/null | head -1); \
	  FONT2=$$(find /usr/share/fonts -type f \( -iname 'NotoSansArabic-Regular.ttf' -o -iname 'NotoNaskhArabic-Regular.ttf' \) 2>/dev/null | head -1); \
	  if [ -z "$$FONT" ]; then FONT=$$(find /usr/share/fonts -type f -iname '*.ttf' 2>/dev/null | head -1); fi; \
	  if [ -n "$$FONT" ]; then \
	    $(CC) -O2 -DTTF_HAVE_ICU $$(pkg-config --cflags icu-uc) -Imods/render/ttf_render tests/text/shaping_test.c mods/render/ttf_render/shaping.c -o build/text_shaping_test $$(pkg-config --libs icu-uc) -ldl && build/text_shaping_test "$$FONT" $$([ -n "$$FONT2" ] && printf '%s' "$$FONT2"); \
	  else echo 'SKIP: no suitable system font'; fi; \
	else echo 'SKIP: ICU development package unavailable'; fi
.PHONY: test-text-shaping

# Browser-facing JS API integration tests (requires Linux + xvfb-run).
test-browser-apis: $(TARGET) test-quickjs-jit build/webrtc_turn_server_fixture
	bash tests/dom_js.sh
	bash tests/worker_clone.sh
	bash tests/service_worker.sh
	WAIT=45 bash tests/webrtc_browser.sh
	bash tests/webrtc_turn_browser.sh

.PHONY: test-browser-apis

# JavaScript-level RTCPeerConnection + DCEP/DataChannel in-process integration.
test-webrtc-browser: $(TARGET) build/webrtc_turn_server_fixture
	WAIT=45 bash tests/webrtc_browser.sh
	bash tests/webrtc_turn_browser.sh

.PHONY: test-webrtc-browser

# Native, dependency-free STUN/UDP test suite for the in-tree WebRTC foundation.
test-webrtc-stun: build/webrtc_stun_test
	./build/webrtc_stun_test

build/webrtc_stun_test: tests/webrtc/stun_test.c mods/render/webpage/webrtc_stun.c mods/render/webpage/webrtc_stun.h
	@mkdir -p build
	$(CC) -Wall -Wextra -Werror -O2 -I mods/render/webpage tests/webrtc/stun_test.c mods/render/webpage/webrtc_stun.c -o $@ -pthread

.PHONY: test-webrtc-stun

# In-tree ICE protocol primitives: candidate parsing, secure credentials,
# authenticated STUN checks/responses. The OpenSSL provider is optional, as in
# the main build; cryptographic ICE operations fail closed when unavailable.
test-webrtc-ice: build/webrtc_ice_test
	./build/webrtc_ice_test

build/webrtc_ice_test: tests/webrtc/ice_test.c mods/render/webpage/webrtc_ice.c mods/render/webpage/webrtc_ice.h mods/render/webpage/webrtc_stun.c mods/render/webpage/webrtc_stun.h
	mkdir -p build
	$(CC) -Wall -Wextra -Werror -Wno-deprecated-declarations -O2 $(shell pkg-config --cflags openssl 2>/dev/null) $(shell pkg-config --exists openssl && echo -DHAVE_OPENSSL_VAULT) -I mods/render/webpage tests/webrtc/ice_test.c mods/render/webpage/webrtc_ice.c mods/render/webpage/webrtc_stun.c $(shell pkg-config --libs openssl 2>/dev/null) -o $@ -pthread

.PHONY: test-webrtc-ice

# UDP ICE agent integration tests: host candidate gathering, STUN transactions,
# checklist pacing/retransmission, role conflict, nomination and datagram handoff.
test-webrtc-agent: build/webrtc_ice_agent_test
	./build/webrtc_ice_agent_test

build/webrtc_ice_agent_test: tests/webrtc/ice_agent_test.c mods/render/webpage/webrtc_ice_agent.c mods/render/webpage/webrtc_ice_agent.h mods/render/webpage/webrtc_turn.c mods/render/webpage/webrtc_turn.h mods/render/webpage/webrtc_ice.c mods/render/webpage/webrtc_ice.h mods/render/webpage/webrtc_stun.c mods/render/webpage/webrtc_stun.h
	mkdir -p build
	$(CC) -Wall -Wextra -Werror -Wno-deprecated-declarations -O2 $(shell pkg-config --cflags openssl 2>/dev/null) $(shell pkg-config --exists openssl && echo -DHAVE_OPENSSL_VAULT) -I mods/render/webpage tests/webrtc/ice_agent_test.c mods/render/webpage/webrtc_ice_agent.c mods/render/webpage/webrtc_turn.c mods/render/webpage/webrtc_ice.c mods/render/webpage/webrtc_stun.c $(shell pkg-config --libs openssl 2>/dev/null) -o $@ -pthread

.PHONY: test-webrtc-agent

# TURN/UDP client transactions: authentication challenges, allocation lifecycle,
# permissions, channel bindings, Send/Data Indications and ChannelData framing.
test-webrtc-turn: build/webrtc_turn_test
	./build/webrtc_turn_test

build/webrtc_turn_test: tests/webrtc/turn_test.c mods/render/webpage/webrtc_turn.c mods/render/webpage/webrtc_turn.h mods/render/webpage/webrtc_stun.h
	@mkdir -p build
	$(CC) -Wall -Wextra -Werror -Wno-deprecated-declarations -O2 $(shell pkg-config --cflags openssl 2>/dev/null) $(shell pkg-config --exists openssl && echo -DHAVE_OPENSSL_VAULT) -I mods/render/webpage tests/webrtc/turn_test.c mods/render/webpage/webrtc_turn.c $(shell pkg-config --libs openssl 2>/dev/null) -o $@ -pthread

.PHONY: test-webrtc-turn

# End-to-end ICE checklist integration through an in-process TURN relay.
test-webrtc-turn-ice: build/webrtc_turn_ice_integration_test
	./build/webrtc_turn_ice_integration_test

build/webrtc_turn_ice_integration_test: tests/webrtc/turn_ice_integration_test.c mods/render/webpage/webrtc_turn.c mods/render/webpage/webrtc_turn.h mods/render/webpage/webrtc_ice_agent.c mods/render/webpage/webrtc_ice_agent.h mods/render/webpage/webrtc_ice.c mods/render/webpage/webrtc_ice.h mods/render/webpage/webrtc_stun.c mods/render/webpage/webrtc_stun.h
	@mkdir -p build
	$(CC) -Wall -Wextra -Werror -Wno-deprecated-declarations -O2 $(shell pkg-config --cflags openssl 2>/dev/null) $(shell pkg-config --exists openssl && echo -DHAVE_OPENSSL_VAULT) -I mods/render/webpage tests/webrtc/turn_ice_integration_test.c mods/render/webpage/webrtc_turn.c mods/render/webpage/webrtc_ice_agent.c mods/render/webpage/webrtc_ice.c mods/render/webpage/webrtc_stun.c $(shell pkg-config --libs openssl 2>/dev/null) -o $@ -pthread

.PHONY: test-webrtc-turn-ice

test-webrtc-dtls: build/webrtc_dtls_test
	./build/webrtc_dtls_test

build/webrtc_dtls_test: tests/webrtc/dtls_test.c mods/render/webpage/webrtc_dtls.c mods/render/webpage/webrtc_dtls.h mods/render/webpage/webrtc_ice_agent.c mods/render/webpage/webrtc_ice_agent.h mods/render/webpage/webrtc_turn.c mods/render/webpage/webrtc_turn.h mods/render/webpage/webrtc_ice.c mods/render/webpage/webrtc_ice.h mods/render/webpage/webrtc_stun.c mods/render/webpage/webrtc_stun.h
	mkdir -p build
	$(CC) -Wall -Wextra -Werror -Wno-deprecated-declarations -O2 $(shell pkg-config --cflags openssl 2>/dev/null) $(shell pkg-config --exists openssl && echo -DHAVE_OPENSSL_VAULT) -I mods/render/webpage tests/webrtc/dtls_test.c mods/render/webpage/webrtc_dtls.c mods/render/webpage/webrtc_ice_agent.c mods/render/webpage/webrtc_turn.c mods/render/webpage/webrtc_ice.c mods/render/webpage/webrtc_stun.c $(shell pkg-config --libs openssl 2>/dev/null) -o $@ -pthread

.PHONY: test-webrtc-dtls

test-webrtc: test-webrtc-stun test-webrtc-ice test-webrtc-agent test-webrtc-turn test-webrtc-turn-ice test-webrtc-dtls test-webrtc-sctp test-webrtc-datachannel test-webrtc-peer test-webrtc-peer-turn

.PHONY: test-webrtc


# Experimental in-tree SCTP association over the existing fingerprint-pinned DTLS transport.
test-webrtc-sctp: build/webrtc_sctp_test
	./build/webrtc_sctp_test

build/webrtc_sctp_test: tests/webrtc/sctp_test.c mods/render/webpage/webrtc_sctp.c mods/render/webpage/webrtc_sctp.h mods/render/webpage/webrtc_dtls.c mods/render/webpage/webrtc_dtls.h mods/render/webpage/webrtc_ice_agent.c mods/render/webpage/webrtc_ice_agent.h mods/render/webpage/webrtc_turn.c mods/render/webpage/webrtc_turn.h mods/render/webpage/webrtc_ice.c mods/render/webpage/webrtc_ice.h mods/render/webpage/webrtc_stun.c mods/render/webpage/webrtc_stun.h
	@mkdir -p build
	$(CC) -Wall -Wextra -Werror -Wno-deprecated-declarations -O2 $(shell pkg-config --cflags openssl 2>/dev/null) $(shell pkg-config --exists openssl && echo -DHAVE_OPENSSL_VAULT) -DMLB_SCTP_TESTING -I mods/render/webpage tests/webrtc/sctp_test.c mods/render/webpage/webrtc_sctp.c mods/render/webpage/webrtc_dtls.c mods/render/webpage/webrtc_ice_agent.c mods/render/webpage/webrtc_turn.c mods/render/webpage/webrtc_ice.c mods/render/webpage/webrtc_stun.c $(shell pkg-config --libs openssl 2>/dev/null) -o $@ -pthread

.PHONY: test-webrtc-sctp

# Guarded x86-64 QuickJS baseline-JIT checks, including native-tier counters and deoptimization.
QUICKJS_CORE_OBJS = $(OBJDIR)/mods/frame/quickjs/quickjs.o $(OBJDIR)/mods/frame/quickjs/libregexp.o $(OBJDIR)/mods/frame/quickjs/libunicode.o $(OBJDIR)/mods/frame/quickjs/cutils.o $(OBJDIR)/mods/frame/quickjs/dtoa.o

test-quickjs-jit: $(QUICKJS_CORE_OBJS) tests/js_engine/quickjs_jit_check.c
	$(CC) $(CFLAGS) -Imods/frame/quickjs tests/js_engine/quickjs_jit_check.c $(QUICKJS_CORE_OBJS) -o $(OBJDIR)/quickjs_jit_test -lm -ldl -lpthread
	./$(OBJDIR)/quickjs_jit_test

.PHONY: test-quickjs-jit

# QuickJS source-line instrumentation contract (used by the DevTools trace panel).
test-quickjs-trace: $(QUICKJS_CORE_OBJS) tests/js_engine/trace_hook_check.c
	$(CC) $(CFLAGS) -Imods/frame/quickjs tests/js_engine/trace_hook_check.c $(QUICKJS_CORE_OBJS) -o $(OBJDIR)/quickjs_trace_test -lm -ldl -lpthread
	./$(OBJDIR)/quickjs_trace_test

.PHONY: test-quickjs-trace

# End-to-end pausing debugger handshake with the actual browser under Xvfb.
test-debugger-pause: $(TARGET) tests/js_engine/send_debugger_key.c tests/js_engine/debugger_pause_integration.sh tests/pages/debugger_pause_fixture.html
	bash tests/js_engine/debugger_pause_integration.sh

.PHONY: test-debugger-pause

# Non-blocking, process-isolated DOM IPC: fire-and-queue a non-cancelable event,
# poll its reply without waiting, then verify the DOM snapshot is applied.
test-async-dom-ipc: $(TEST_OBJS) tests/js_engine/async_dom_ipc_check.c main.c
	$(CC) $(CFLAGS) -DMLBROWSER_TEST_AUTO_CONTINUE -I. tests/js_engine/async_dom_ipc_check.c $(TEST_OBJS) -o $(OBJDIR)/async_dom_ipc_test $(LIBS)
	@tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; mkdir -p "$$tmp/cache"; \
	  (cd "$$tmp" && "$(CURDIR)/$(OBJDIR)/async_dom_ipc_test")

.PHONY: test-async-dom-ipc

# Reproducible interpreter-vs-native baseline JIT microbenchmark.
bench-quickjs-jit: $(QUICKJS_CORE_OBJS) tests/js_engine/quickjs_jit_bench.c
	$(CC) $(CFLAGS) -Imods/frame/quickjs tests/js_engine/quickjs_jit_bench.c $(QUICKJS_CORE_OBJS) -o $(OBJDIR)/quickjs_jit_bench -lm -ldl -lpthread
	./$(OBJDIR)/quickjs_jit_bench

.PHONY: bench-quickjs-jit

# BigInt short-value baseline JIT microbenchmark, separate from Number/Float64 workloads.
bench-quickjs-jit-bigint: $(QUICKJS_CORE_OBJS) tests/js_engine/quickjs_jit_bigint_bench.c
	$(CC) $(CFLAGS) -Imods/frame/quickjs tests/js_engine/quickjs_jit_bigint_bench.c $(QUICKJS_CORE_OBJS) -o $(OBJDIR)/quickjs_jit_bigint_bench -lm -ldl -lpthread
	./$(OBJDIR)/quickjs_jit_bigint_bench

.PHONY: bench-quickjs-jit-bigint

# Microbenchmark for native helper-backed arbitrary-precision heap BigInts.
bench-quickjs-jit-heap-bigint: $(QUICKJS_CORE_OBJS) tests/js_engine/quickjs_jit_heap_bigint_bench.c
	$(CC) $(CFLAGS) -Imods/frame/quickjs tests/js_engine/quickjs_jit_heap_bigint_bench.c $(QUICKJS_CORE_OBJS) -o $(OBJDIR)/quickjs_jit_heap_bigint_bench -lm -ldl -lpthread
	./$(OBJDIR)/quickjs_jit_heap_bigint_bench

.PHONY: bench-quickjs-jit-heap-bigint

# User-agent-created <audio>/<video> controls and frame callback lifecycle.
test-media-controls: $(TARGET)
	bash tests/media_controls.sh

# Bounded SPSC byte queue used as the input substrate for future MSE demuxing.
test-media-stream-queue:
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) -Imods/render/webpage tests/media/byte_queue_check.c mods/render/webpage/media_byte_queue.c -o $(OBJDIR)/media_byte_queue_test -pthread
	./$(OBJDIR)/media_byte_queue_test

.PHONY: test-media-stream-queue

test-media-stream-backend:
	@command -v ffmpeg >/dev/null || { echo "SKIP: ffmpeg not installed"; exit 0; }
	@mkdir -p $(OBJDIR) tests/media
	$(CC) -Wall -Wextra -O2 -Imods/render/webpage tests/media/stream_backend_check.c mods/render/webpage/media_backend.c mods/render/webpage/media_byte_queue.c -lm -pthread -o $(OBJDIR)/media_stream_backend_test
	ffmpeg -y -hide_banner -loglevel error -f lavfi -i color=c=red:s=32x32:r=8:d=1 -f lavfi -i sine=frequency=440:duration=1 -shortest -c:v mpeg4 -pix_fmt yuv420p -c:a aac -b:a 64k -movflags +frag_keyframe+empty_moov+default_base_moof tests/media/test-stream.mp4
	SDL_AUDIODRIVER=dummy ./$(OBJDIR)/media_stream_backend_test tests/media/test-stream.mp4; rc=$$?; rm -f tests/media/test-stream.mp4; exit $$rc

.PHONY: test-media-stream-backend

test-media-stream-bridge: $(TARGET)
	bash tests/media_stream_bridge.sh

.PHONY: test-media-stream-bridge

.PHONY: test-media-controls

test-media-pipeline: $(TARGET)
	bash tests/media_pipeline.sh

.PHONY: test-media-pipeline

# Experimental MediaSource/SourceBuffer append-only fMP4 path through native streaming decoder.
test-media-mse: $(TARGET)
	bash tests/media_mse.sh

.PHONY: test-media-mse

# Induce a browser poll stall while SDL continues consuming buffered audio, then
# verify stale PTS frames are dropped instead of presented far behind the master clock.
test-media-av-late-frame-drop:
	@command -v ffmpeg >/dev/null && command -v ffprobe >/dev/null || { echo "SKIP: FFmpeg tools not installed"; exit 0; }
	@mkdir -p $(OBJDIR) tests/media
	$(CC) -Wall -Wextra -O2 -Imods/render/webpage tests/media/av_late_frame_drop_check.c mods/render/webpage/media_backend.c mods/render/webpage/media_byte_queue.c -lm -ldl -o $(OBJDIR)/media_av_late_frame_drop_test
	ffmpeg -y -hide_banner -loglevel error -f lavfi -i testsrc2=size=64x64:rate=30:duration=3 -f lavfi -i sine=frequency=660:duration=3 -shortest -c:v mpeg4 -g 30 -pix_fmt yuv420p -c:a aac -b:a 64k -movflags +faststart tests/media/test-av-late-frame-drop.mp4
	SDL_AUDIODRIVER=dummy ./$(OBJDIR)/media_av_late_frame_drop_test tests/media/test-av-late-frame-drop.mp4; rc=$$?; rm -f tests/media/test-av-late-frame-drop.mp4; exit $$rc

.PHONY: test-media-av-late-frame-drop

# Active file seek events complete only after the restarted decoder produces usable data.
test-media-seek-events: $(TARGET)
	bash tests/media_seek_events.sh

.PHONY: test-media-seek-events

# Accurate file seeks must preserve FFmpeg's first post-target frame PTS offset.
test-media-file-seek-pts:
	@command -v ffmpeg >/dev/null && command -v ffprobe >/dev/null || { echo "SKIP: FFmpeg tools not installed"; exit 0; }
	@mkdir -p $(OBJDIR) tests/media
	$(CC) -Wall -Wextra -O2 -Imods/render/webpage tests/media/file_seek_pts_check.c mods/render/webpage/media_backend.c mods/render/webpage/media_byte_queue.c -lm -ldl -pthread -o $(OBJDIR)/media_file_seek_pts_test
	ffmpeg -y -hide_banner -loglevel error -f lavfi -i testsrc2=size=64x64:rate=10:duration=5 -f lavfi -i sine=frequency=440:duration=5 -shortest -c:v mpeg4 -g 10 -q:v 5 -pix_fmt yuv420p -c:a aac -b:a 64k -movflags +faststart tests/media/test-file-seek-pts.mp4
	SDL_AUDIODRIVER=dummy ./$(OBJDIR)/media_file_seek_pts_test tests/media/test-file-seek-pts.mp4; rc=$$?; rm -f tests/media/test-file-seek-pts.mp4; exit $$rc

.PHONY: test-media-file-seek-pts

# HTMLMediaElement.fastSeek() should choose the preceding demuxer-indexed video keyframe.
test-media-fast-seek-keyframes:
	@command -v ffmpeg >/dev/null && command -v ffprobe >/dev/null || { echo "SKIP: FFmpeg tools not installed"; exit 0; }
	@mkdir -p $(OBJDIR) tests/media
	$(CC) -Wall -Wextra -O2 -DMEDIA_FAST_SEEK_TESTING -Imods/render/webpage tests/media/media_fast_seek_check.c mods/render/webpage/media_backend.c mods/render/webpage/media_byte_queue.c -lm -ldl -pthread -o $(OBJDIR)/media_fast_seek_test
	@set -e; \
	  cache_home=$$(mktemp -d); \
	  trap 'rm -f tests/media/test-media-fast-seek.mp4 tests/media/test-media-fast-seek-cache.mp4 tests/media/test-media-fast-seek-persist.mp4; rm -rf "$$cache_home"' EXIT; \
	  export XDG_CACHE_HOME="$$cache_home"; \
	  ffmpeg -y -hide_banner -loglevel error -f lavfi -i testsrc2=size=64x64:rate=10:duration=8 -c:v mpeg4 -g 30 -q:v 5 -pix_fmt yuv420p -movflags +faststart tests/media/test-media-fast-seek.mp4; \
	  ./$(OBJDIR)/media_fast_seek_test tests/media/test-media-fast-seek.mp4; \
	  ffmpeg -y -hide_banner -loglevel error -f lavfi -i testsrc2=size=64x64:rate=10:duration=8 -c:v mpeg4 -g 10 -q:v 5 -pix_fmt yuv420p -movflags +faststart tests/media/test-media-fast-seek-cache.mp4; \
	  ./$(OBJDIR)/media_fast_seek_test tests/media/test-media-fast-seek-cache.mp4 cache; \
	  ffmpeg -y -hide_banner -loglevel error -f lavfi -i testsrc2=size=64x64:rate=10:duration=8 -c:v mpeg4 -g 10 -q:v 5 -pix_fmt yuv420p -movflags +faststart tests/media/test-media-fast-seek-persist.mp4; \
	  ./$(OBJDIR)/media_fast_seek_test tests/media/test-media-fast-seek-persist.mp4 persist-seed; \
	  ./$(OBJDIR)/media_fast_seek_test tests/media/test-media-fast-seek-persist.mp4 persist-check; \
	  ./$(OBJDIR)/media_fast_seek_test tests/media/test-media-fast-seek-persist.mp4 persist-stale

.PHONY: test-media-fast-seek-keyframes

test-media-fast-seek-browser: $(TARGET)
	bash tests/media_fast_seek_browser.sh

.PHONY: test-media-fast-seek-browser

# DCEP and WebRTC RTCDataChannel transport over the in-tree SCTP/DTLS stack.
test-webrtc-datachannel: build/webrtc_datachannel_test
	./build/webrtc_datachannel_test

build/webrtc_datachannel_test: tests/webrtc/datachannel_test.c mods/render/webpage/webrtc_datachannel.c mods/render/webpage/webrtc_datachannel.h mods/render/webpage/webrtc_sctp.c mods/render/webpage/webrtc_sctp.h mods/render/webpage/webrtc_dtls.c mods/render/webpage/webrtc_dtls.h mods/render/webpage/webrtc_ice_agent.c mods/render/webpage/webrtc_ice_agent.h mods/render/webpage/webrtc_turn.c mods/render/webpage/webrtc_turn.h mods/render/webpage/webrtc_ice.c mods/render/webpage/webrtc_ice.h mods/render/webpage/webrtc_stun.c mods/render/webpage/webrtc_stun.h
	@mkdir -p build
	$(CC) -Wall -Wextra -Werror -Wno-deprecated-declarations -O2 $(shell pkg-config --cflags openssl 2>/dev/null) $(shell pkg-config --exists openssl && echo -DHAVE_OPENSSL_VAULT) -I mods/render/webpage tests/webrtc/datachannel_test.c mods/render/webpage/webrtc_datachannel.c mods/render/webpage/webrtc_sctp.c mods/render/webpage/webrtc_dtls.c mods/render/webpage/webrtc_ice_agent.c mods/render/webpage/webrtc_turn.c mods/render/webpage/webrtc_ice.c mods/render/webpage/webrtc_stun.c $(shell pkg-config --libs openssl 2>/dev/null) -o $@ -pthread

.PHONY: test-webrtc-datachannel

# Native RTCPeerConnection backend: SDP offer/answer and ICE->DTLS->SCTP->DCEP.
test-webrtc-peer: build/webrtc_peer_backend_test
	./build/webrtc_peer_backend_test

build/webrtc_peer_backend_test: tests/webrtc/peer_backend_test.c mods/render/webpage/webrtc_backend.c mods/render/webpage/webrtc_backend.h mods/render/webpage/webrtc_datachannel.c mods/render/webpage/webrtc_sctp.c mods/render/webpage/webrtc_dtls.c mods/render/webpage/webrtc_ice_agent.c mods/render/webpage/webrtc_turn.c mods/render/webpage/webrtc_ice.c mods/render/webpage/webrtc_stun.c
	@mkdir -p build
	$(CC) -Wall -Wextra -Werror -Wno-deprecated-declarations -O2 $(shell pkg-config --cflags openssl 2>/dev/null) $(shell pkg-config --exists openssl && echo -DHAVE_OPENSSL_VAULT) -I mods/render/webpage tests/webrtc/peer_backend_test.c mods/render/webpage/webrtc_backend.c mods/render/webpage/webrtc_datachannel.c mods/render/webpage/webrtc_sctp.c mods/render/webpage/webrtc_dtls.c mods/render/webpage/webrtc_ice_agent.c mods/render/webpage/webrtc_turn.c mods/render/webpage/webrtc_ice.c mods/render/webpage/webrtc_stun.c $(shell pkg-config --libs openssl 2>/dev/null) -o $@ -pthread

.PHONY: test-webrtc-peer

# End-to-end RTCConfiguration TURN relay integration through the native peer backend.
test-webrtc-peer-turn: build/webrtc_peer_turn_backend_test
	./build/webrtc_peer_turn_backend_test

build/webrtc_peer_turn_backend_test: tests/webrtc/peer_turn_backend_test.c tests/webrtc/turn_ice_integration_test.c mods/render/webpage/webrtc_backend.c mods/render/webpage/webrtc_backend.h mods/render/webpage/webrtc_datachannel.c mods/render/webpage/webrtc_datachannel.h mods/render/webpage/webrtc_sctp.c mods/render/webpage/webrtc_sctp.h mods/render/webpage/webrtc_dtls.c mods/render/webpage/webrtc_dtls.h mods/render/webpage/webrtc_ice_agent.c mods/render/webpage/webrtc_ice_agent.h mods/render/webpage/webrtc_turn.c mods/render/webpage/webrtc_turn.h mods/render/webpage/webrtc_ice.c mods/render/webpage/webrtc_ice.h mods/render/webpage/webrtc_stun.c mods/render/webpage/webrtc_stun.h
	@mkdir -p build
	$(CC) -Wall -Wextra -Werror -Wno-deprecated-declarations -O2 $(shell pkg-config --cflags openssl 2>/dev/null) $(shell pkg-config --exists openssl && echo -DHAVE_OPENSSL_VAULT) -I mods/render/webpage tests/webrtc/peer_turn_backend_test.c mods/render/webpage/webrtc_backend.c mods/render/webpage/webrtc_datachannel.c mods/render/webpage/webrtc_sctp.c mods/render/webpage/webrtc_dtls.c mods/render/webpage/webrtc_ice_agent.c mods/render/webpage/webrtc_turn.c mods/render/webpage/webrtc_ice.c mods/render/webpage/webrtc_stun.c $(shell pkg-config --libs openssl 2>/dev/null) -o $@ -pthread

.PHONY: test-webrtc-peer-turn

# Local TURN/UDP simulator for the full JavaScript relay-only RTCPeerConnection test.
build/webrtc_turn_server_fixture: tests/webrtc/turn_server_fixture.c tests/webrtc/turn_ice_integration_test.c mods/render/webpage/webrtc_turn.c mods/render/webpage/webrtc_turn.h mods/render/webpage/webrtc_ice_agent.c mods/render/webpage/webrtc_ice_agent.h mods/render/webpage/webrtc_ice.c mods/render/webpage/webrtc_ice.h mods/render/webpage/webrtc_stun.c mods/render/webpage/webrtc_stun.h
	@mkdir -p build
	$(CC) -Wall -Wextra -Werror -Wno-deprecated-declarations -O2 $(shell pkg-config --cflags openssl 2>/dev/null) $(shell pkg-config --exists openssl && echo -DHAVE_OPENSSL_VAULT) -I mods/render/webpage tests/webrtc/turn_server_fixture.c mods/render/webpage/webrtc_turn.c mods/render/webpage/webrtc_ice_agent.c mods/render/webpage/webrtc_ice.c mods/render/webpage/webrtc_stun.c $(shell pkg-config --libs openssl 2>/dev/null) -o $@ -pthread

.PHONY: test-webrtc-turn-browser
