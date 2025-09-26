#include "uv_internal.h"
#include <stdarg.h>

void uv_log_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    g_logv("uv-viewer", G_LOG_LEVEL_INFO, fmt, ap);
    va_end(ap);
}

void uv_log_warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    g_logv("uv-viewer", G_LOG_LEVEL_WARNING, fmt, ap);
    va_end(ap);
}

void uv_log_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    g_logv("uv-viewer", G_LOG_LEVEL_CRITICAL, fmt, ap);
    va_end(ap);
}
