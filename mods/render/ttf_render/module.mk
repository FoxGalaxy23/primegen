MOD_NAME := ttf_render
MOD_SRCS += $(MOD_PATH)/start.c $(MOD_PATH)/woff2.c $(MOD_PATH)/brotli.c $(MOD_PATH)/cff.c
MOD_CFLAGS += -I$(MOD_PATH)
