MOD_NAME := nal_linux
# собственный HTTP/1.1-клиент + TLS (OpenSSL через dlopen) + inflate + политика CORS; общий код лежит в mods/nal/common
MOD_SRCS += $(MOD_PATH)/start.c $(MOD_PATH)/../common/nhttp.c $(MOD_PATH)/../common/ncors.c $(MOD_PATH)/../common/psl.c $(MOD_PATH)/../common/ntls_openssl.c $(MOD_PATH)/../common/ninflate.c
MOD_CFLAGS += -I$(MOD_PATH)/../common
