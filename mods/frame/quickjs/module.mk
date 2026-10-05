# QuickJS (https://bellard.org/quickjs/, MIT) — ядро языка JavaScript для страниц.
MOD_NAME := quickjs
MOD_SRCS += $(MOD_PATH)/quickjs.c $(MOD_PATH)/libregexp.c $(MOD_PATH)/libunicode.c $(MOD_PATH)/cutils.c $(MOD_PATH)/dtoa.c
MOD_CFLAGS += -D_GNU_SOURCE -DCONFIG_VERSION=\"2026-06-04\" -DHAVE_QUICKJS
ifneq ($(PLATFORM),win32)
    LIBS += -lpthread
endif
# сторонний код: без шума предупреждений проекта
$(OBJDIR)/$(MOD_PATH)/%.o: CFLAGS += -Wno-sign-compare -Wno-unused-parameter -Wno-missing-field-initializers -Wno-implicit-fallthrough -Wno-unused-function -Wno-cast-function-type -Wno-array-bounds -Wno-format-truncation -Wno-stringop-truncation -Wno-unused-but-set-variable
