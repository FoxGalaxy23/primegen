MOD_NAME := webpage
MOD_SRCS += $(MOD_PATH)/html.c $(MOD_PATH)/css.c $(MOD_PATH)/style.c $(MOD_PATH)/images.c $(MOD_PATH)/svg.c $(MOD_PATH)/layout2.c $(MOD_PATH)/paint.c $(MOD_PATH)/fx.c $(MOD_PATH)/anim.c $(MOD_PATH)/js.c $(MOD_PATH)/js_prelude.c $(MOD_PATH)/ws.c $(MOD_PATH)/worker.c
MOD_CFLAGS += -DSVG_WITH_TTF
