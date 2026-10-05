#ifndef LOGGER_H
#define LOGGER_H

#include <stdarg.h>

#ifndef DISABLED_MOD_LOGGER

// Основной логгер (вызывается через gal_log)
void custom_logger(const char* level, const char* format, va_list args);

// Установить обработчик сигналов SIGSEGV/SIGABRT/SIGFPE
// чтобы при краше причина записывалась в browser.log
void logger_init_crash_handler(void);

#else

static inline void custom_logger(const char* level, const char* format, va_list args) { (void)level; (void)format; (void)args; }
static inline void logger_init_crash_handler(void) {}

#endif

#endif
