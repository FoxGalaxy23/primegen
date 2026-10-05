#include "logger.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <stdlib.h>
#ifndef _WIN32
#include <unistd.h>
#endif

// Внутренняя функция записи одной строки в лог (без va_list)
static void log_raw(const char* level, const char* msg) {
    FILE* log_file = fopen("browser.log", "a");
    time_t rawtime;
    struct tm* timeinfo;
    char time_str[20];

    time(&rawtime);
    timeinfo = localtime(&rawtime);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", timeinfo);

    FILE* stream = (strcmp(level, "ERROR") == 0 || strcmp(level, "WARN") == 0) ? stderr : stdout;
    fprintf(stream, "[%s] [%s] %s\n", time_str, level, msg);
    fflush(stream);

    if (log_file) {
        fprintf(log_file, "[%s] [%s] %s\n", time_str, level, msg);
        fflush(log_file);
        fclose(log_file);
    }
}

void custom_logger(const char* level, const char* format, va_list args) {
    // Форматируем строку сначала в буфер
    char buf[2048];
    vsnprintf(buf, sizeof(buf), format, args);
    log_raw(level, buf);
}

// ───── Обработчик сигналов ─────────────────────────────────────────────────

static const char* signal_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (Segmentation fault — выход за границы памяти)";
        case SIGABRT: return "SIGABRT (Abort — assert/abort() внутри программы)";
        case SIGFPE:  return "SIGFPE  (Floating point / деление на ноль)";
#ifdef SIGBUS
        case SIGBUS:  return "SIGBUS  (Bus error — невыровненный доступ к памяти)";
#endif
        case SIGILL:  return "SIGILL  (Illegal instruction)";
#ifdef SIGPIPE
        case SIGPIPE: return "SIGPIPE (Broken pipe — сокет закрыт на другом конце)";
#endif
        default:      return "Неизвестный сигнал";
    }
}

static void crash_handler(int sig) {
    // Важно: в обработчике сигнала можно использовать только async-safe функции.
    // Мы делаем минимальную запись через write() и fopen/fprintf (достаточно для диагностики).
    char msg[256];
    snprintf(msg, sizeof(msg), "!!! CRASH: получен сигнал %d — %s", sig, signal_name(sig));
    log_raw("ERROR", msg);

    // Пишем в stderr напрямую для гарантии
#ifndef _WIN32
    write(STDERR_FILENO, msg, strlen(msg));
    write(STDERR_FILENO, "\n", 1);
#else
    fprintf(stderr, "%s\n", msg);
#endif

    // Восстанавливаем дефолтный обработчик и повторно посылаем сигнал
    // чтобы получить нормальный core dump
    signal(sig, SIG_DFL);
    raise(sig);
}

#ifdef SIGPIPE
static void sigpipe_handler(int sig) {
    (void)sig;
    log_raw("WARN", "SIGPIPE: соединение разорвано сервером (игнорируем)");
}
#endif

void logger_init_crash_handler(void) {
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGFPE,  crash_handler);
#ifdef SIGBUS
    signal(SIGBUS,  crash_handler);
#endif
    signal(SIGILL,  crash_handler);
    // SIGPIPE — не крашим, просто логируем и продолжаем
#ifdef SIGPIPE
    signal(SIGPIPE, sigpipe_handler);
#endif

    log_raw("INFO", "Crash handler установлен.");
}