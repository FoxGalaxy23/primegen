MOD_NAME := nal_win32
MOD_SRCS += $(MOD_PATH)/start.c $(MOD_PATH)/../common/nal_monitor.c $(MOD_PATH)/../common/idna.c $(MOD_PATH)/../common/web_security.c

MOD_CFLAGS += -I$(MOD_PATH)/../common
