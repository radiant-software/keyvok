# Builds keyvok.exe from WSL with clang-cl, against the MSVC toolset and
# Windows SDK installed on the Windows side. vcvars64.bat is not needed:
# the toolchain directories are passed to clang-cl directly.
#
#   make           release build: build/release/keyvok.exe
#   make debug     debug build:   build/debug/keyvok.exe (console, CRT leak report)
#   make test      build and run the storage-layer tests (CONFIG=debug for a debug build)
#   make test-clipboard  clipboard tests: replace the clipboard's contents, take ~30 s
#   make seed      create build/$(CONFIG)/keyvok.db with fake accounts (test/seed.csv)
#   make screenshots  retake docs/images/*.png from demo accounts; close Keyvok first
#   make sanitize  run the tests under ASan + UBSan: build/sanitize/
#   make clean

# Full paths: a plain clang-cl on PATH can resolve to Swift's toolchain.
CC := /usr/bin/clang-cl-22
RC := /usr/lib/llvm-22/bin/llvm-rc

# Update these versions when Visual Studio or the Windows SDK updates.
MSVC_DIR    := /mnt/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207
SDK_DIR     := /mnt/c/Program Files (x86)/Windows Kits/10
SDK_VERSION := 10.0.26100.0
SDK_INCLUDE := $(SDK_DIR)/Include/$(SDK_VERSION)

TOOLCHAIN := /nologo -fuse-ld=lld /vctoolsdir "$(MSVC_DIR)" /winsdkdir "$(SDK_DIR)" /winsdkversion $(SDK_VERSION)

CONFIG ?= release
OUT    := build/$(CONFIG)

# release and debug link the CRT statically, so keyvok.exe needs no runtime
# DLLs. sanitize uses the dynamic CRT, because MSVC ships ASan only as a DLL.
ifeq ($(CONFIG),release)
  CRT       := /MT
  APP_OPT   := /O2 /DNDEBUG
  SUBSYSTEM := /SUBSYSTEM:WINDOWS
else ifeq ($(CONFIG),debug)
  CRT       := /MTd
  APP_OPT   := /Od /Zi /DKEYVOK_DEBUG
  SUBSYSTEM := /SUBSYSTEM:CONSOLE /ENTRY:wWinMainCRTStartup /DEBUG
else ifeq ($(CONFIG),sanitize)
  # UBSan in trap mode needs no runtime: undefined behavior stops the
  # program at the faulting instruction. ASan uses MSVC's runtime DLL,
  # which is copied next to each .exe.
  CRT       := /MD
  SAN       := -fsanitize=address
  APP_OPT   := /O1 /Zi $(SAN) -fsanitize=undefined -fsanitize-trap=undefined /Oy-
  SUBSYSTEM := /SUBSYSTEM:CONSOLE /ENTRY:wWinMainCRTStartup /DEBUG
  ASAN_DLL  := $(OUT)/clang_rt.asan_dynamic-x86_64.dll
  # clang-cl would link ASan libraries from LLVM's resource dir, which the
  # Linux LLVM packages leave out, so link MSVC's copies by hand instead.
  # /OPT:NOICF: folding identical constants (e.g. sqlite's "x\0" and a test's
  # L"x") makes ASan poison the tail of the longer one: a false overflow.
  ASAN_LINK := /OPT:NOICF clang_rt.asan_dynamic-x86_64.lib /include:__asan_seh_interceptor \
	/wholearchive:clang_rt.asan_dynamic_runtime_thunk-x86_64.lib
else
  $(error CONFIG must be release, debug or sanitize)
endif

# SQLite features keyvok does not use. The app is compiled with them too,
# so sqlite3.h agrees with the library; it hides the deprecated APIs.
SQLITE_OMIT := \
	-DSQLITE_OMIT_DEPRECATED \
	-DSQLITE_OMIT_LOAD_EXTENSION \
	-DSQLITE_OMIT_JSON \
	-DSQLITE_OMIT_PROGRESS_CALLBACK \
	-DSQLITE_OMIT_AUTOINIT

APP_CFLAGS := /std:clatest $(CRT) $(APP_OPT) \
	/W4 -Werror -Wswitch-enum -Wshorten-64-to-32 \
	-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -D_WIN32_WINNT=0x0A00 \
	$(SQLITE_OMIT) \
	/Isrc /imsvc third_party/sqlite /imsvc third_party/qrcodegen

# Makes SQL strict: double quotes never make strings, and foreign keys are
# enforced. Optimized for size in every build: it is most of the exe, and
# /O2 is no faster at keyvok's few small queries.
SQLITE_CFLAGS := $(CRT) /O1 /Gy /Gw $(SQLITE_OMIT) \
	-DSQLITE_DQS=0 \
	-DSQLITE_DEFAULT_FOREIGN_KEYS=1

QRCODEGEN_CFLAGS := $(CRT) /O2 /Gy /Gw /DNDEBUG

# ASan also instruments the libraries, to catch overruns of buffers the app
# passes to them. UBSan covers only the app's own code.
SQLITE_CFLAGS    += $(SAN)
QRCODEGEN_CFLAGS += $(SAN)

LIBS := user32.lib gdi32.lib comctl32.lib uxtheme.lib crypt32.lib bcrypt.lib \
	advapi32.lib ole32.lib shell32.lib shcore.lib uuid.lib

APP_OBJS := $(patsubst src/%.c,$(OUT)/%.obj,$(wildcard src/*.c))
# The modules with no UI, which the tests link.
CORE_OBJS := $(addprefix $(OUT)/,db.obj secret.obj sq.obj clipboard.obj)
OBJS     := $(APP_OBJS) $(OUT)/sqlite3.obj $(OUT)/qrcodegen.obj $(OUT)/resource.res

.PHONY: all debug sanitize test test-clipboard seed screenshots clean
all: $(OUT)/keyvok.exe

debug:
	$(MAKE) CONFIG=debug

sanitize:
	$(MAKE) CONFIG=sanitize test all

$(OUT)/keyvok.exe: $(OBJS) | $(ASAN_DLL)
	$(CC) $(TOOLCHAIN) $(CRT) $^ /Fe:$@ /link $(SUBSYSTEM) $(LIBS) $(ASAN_LINK)

# The tests write their databases to a scratch directory under build/.
test: $(OUT)/db_test.exe
	rm -rf $(OUT)/test-data && mkdir -p $(OUT)/test-data
	$(OUT)/db_test.exe $(OUT)/test-data

test-clipboard: $(OUT)/clipboard_test.exe
	$(OUT)/clipboard_test.exe

# Fake accounts for trying the app by hand. Will not overwrite a database.
seed: $(OUT)/seed.exe
	$(OUT)/seed.exe test/seed.csv $(OUT)/keyvok.db

# The README screenshots. A copy of keyvok.exe in its own folder opens the
# demo database beside it, never the installed app's.
SHOTS := build/screenshots
screenshots: $(OUT)/keyvok.exe $(OUT)/seed.exe
	rm -rf $(SHOTS) && mkdir -p $(SHOTS)
	cp $(OUT)/keyvok.exe $(SHOTS)/
	$(OUT)/seed.exe test/screenshots/accounts.csv $(SHOTS)/keyvok.db
	powershell.exe -NoProfile -ExecutionPolicy Bypass \
		-File "$$(wslpath -w test/screenshots/capture.ps1)" -Dir "$$(wslpath -w $(SHOTS))"
	mkdir -p docs/images
	cp $(SHOTS)/select.png $(SHOTS)/create.png docs/images/

$(OUT)/seed.exe: $(OUT)/seed.obj $(CORE_OBJS) $(OUT)/sqlite3.obj | $(ASAN_DLL)
	$(CC) $(TOOLCHAIN) $(CRT) $^ /Fe:$@ /link /SUBSYSTEM:CONSOLE $(LIBS) $(ASAN_LINK)

$(OUT)/%_test.exe: $(OUT)/%_test.obj $(CORE_OBJS) $(OUT)/sqlite3.obj | $(ASAN_DLL)
	$(CC) $(TOOLCHAIN) $(CRT) $^ /Fe:$@ /link /SUBSYSTEM:CONSOLE $(LIBS) $(ASAN_LINK)

$(OUT)/clang_rt.asan_dynamic-x86_64.dll: | $(OUT)
	cp "$(MSVC_DIR)/bin/Hostx64/x64/clang_rt.asan_dynamic-x86_64.dll" $@

# /clang:-MMD writes a .d file of the project headers each object includes.
$(OUT)/%.obj: src/%.c | $(OUT)
	$(CC) $(TOOLCHAIN) $(APP_CFLAGS) /c $< /Fo$@ /clang:-MMD /clang:-MF$(@:.obj=.d)

$(OUT)/%.obj: test/%.c | $(OUT)
	$(CC) $(TOOLCHAIN) $(APP_CFLAGS) /c $< /Fo$@ /clang:-MMD /clang:-MF$(@:.obj=.d)

$(OUT)/sqlite3.obj: third_party/sqlite/sqlite3.c third_party/sqlite/sqlite3.h | $(OUT)
	$(CC) $(TOOLCHAIN) $(SQLITE_CFLAGS) /c $< /Fo$@

$(OUT)/qrcodegen.obj: third_party/qrcodegen/qrcodegen.c third_party/qrcodegen/qrcodegen.h | $(OUT)
	$(CC) $(TOOLCHAIN) $(QRCODEGEN_CFLAGS) /c $< /Fo$@

# /C 1252: resource.rc spells the copyright sign as a narrow-string escape.
$(OUT)/resource.res: resources/resource.rc src/version.h resources/app.manifest resources/icon.ico | $(OUT)
	$(RC) /nologo /C 1252 /I src /I "$(SDK_INCLUDE)/um" /I "$(SDK_INCLUDE)/shared" \
		/I "$(SDK_INCLUDE)/ucrt" /I "$(MSVC_DIR)/include" /FO $@ $<

$(OUT):
	mkdir -p $@

clean:
	rm -rf build

-include $(wildcard $(OUT)/*.d)
