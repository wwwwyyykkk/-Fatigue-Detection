/* Edge Impulse porting layer for ESP-IDF */
#include "../ei_classifier_porting.h"

#if __has_include("esp_timer.h")
#include "esp_timer.h"
#endif
#if __has_include("esp_log.h")
#include "esp_log.h"
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if !defined(EI_PORTING_ESPIDF)
#define EI_PORTING_ESPIDF 1
#endif

__attribute__((weak)) EI_IMPULSE_ERROR ei_run_impulse_check_canceled(void) {
    return EI_IMPULSE_OK;
}

__attribute__((weak)) EI_IMPULSE_ERROR ei_sleep(int32_t time_ms) {
    usleep((useconds_t)time_ms * 1000);
    return EI_IMPULSE_OK;
}

uint64_t ei_read_timer_ms(void) {
#if __has_include("esp_timer.h")
    return esp_timer_get_time() / 1000;
#else
    return 0;
#endif
}

uint64_t ei_read_timer_us(void) {
#if __has_include("esp_timer.h")
    return esp_timer_get_time();
#else
    return 0;
#endif
}

void ei_serial_set_baudrate(int baudrate) {
    (void)baudrate;
}

__attribute__((weak)) void ei_putchar(char c) {
    putchar(c);
}

__attribute__((weak)) char ei_getchar(void) {
    return (char)getchar();
}

void ei_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

void ei_printf_float(float f) {
    printf("%f", f);
}

__attribute__((weak)) void *ei_malloc(size_t size) {
    return malloc(size);
}

__attribute__((weak)) void *ei_calloc(size_t nitems, size_t size) {
    return calloc(nitems, size);
}

__attribute__((weak)) void ei_free(void *ptr) {
    free(ptr);
}
