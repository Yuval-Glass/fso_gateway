/**
 * @file logging.c
 * @brief Implementation of the FSO Gateway logging module.
 *
 * This file provides a thread-safe logger using a pthread mutex.
 * Each log line includes:
 *
 *   [YYYY-MM-DD HH:MM:SS.ns] [LEVEL] [Thread_ID] message
 *
 * Design goals:
 * - Prevent log line corruption when multiple threads print simultaneously.
 * - Provide nanosecond-resolution timestamps when available.
 * - Keep the API simple for use throughout the FSO Gateway project.
 */

#include "logging.h"

#include <errno.h>      /* errno values for diagnostics if needed */
#include <pthread.h>    /* pthread_mutex_t, pthread_self, mutex operations */
#include <stdint.h>     /* uintptr_t for portable integer-style thread ID formatting */
#include <stdio.h>      /* fprintf, snprintf, vfprintf, stdout, stderr */
#include <stdlib.h>     /* general utilities */
#include <string.h>     /* strerror */
#include <time.h>       /* clock_gettime, localtime_r, struct timespec, struct tm */

/**
 * @brief Global mutex protecting the logging critical section.
 *
 * Why this exists:
 * Standard I/O functions such as fprintf() may be thread-safe at the library
 * level, but that does not guarantee that a multi-step sequence of operations
 * forming one logical log line will remain grouped together. Without our own
 * mutex, one thread could print the timestamp while another thread prints its
 * level/message, causing interleaved and unreadable logs.
 *
 * By locking this mutex around the entire formatting and output sequence,
 * we ensure each complete log entry is emitted atomically from the point
 * of view of other threads in this process.
 */
static pthread_mutex_t g_log_mutex;

/**
 * @brief Indicates whether the logger has been initialized.
 *
 * This flag helps guard against use before initialization and against
 * accidental double shutdown.
 */
static int g_log_initialized = 0;

/**
 * @brief Current minimum severity threshold for emitted messages.
 *
 * Messages with a level numerically lower than this threshold are suppressed.
 * The default remains DEBUG so existing workflows preserve current verbosity
 * unless a caller explicitly raises the threshold.
 */
static log_level g_log_min_level = DEBUG;

/**
 * @brief Convert a log level enum value to a printable string.
 *
 * @param level The log level to convert.
 *
 * @return A constant string representation of the log level.
 */
static const char *log_level_to_string(log_level level)
{
    switch (level) {
        case DEBUG: return "DEBUG";
        case INFO:  return "INFO";
        case WARN:  return "WARN";
        case ERROR: return "ERROR";
        default:    return "UNKNOWN";
    }
}

/**
 * @brief Build a timestamp string with nanosecond resolution if available.
 *
 * This function uses clock_gettime() to obtain wall-clock time with
 * sub-second precision. The seconds field is then converted into a
 * human-readable local time using localtime_r(), which is the reentrant
 * thread-safe variant of localtime().
 *
 * Why use clock_gettime():
 * - It provides higher resolution than time().
 * - It exposes nanoseconds via tv_nsec.
 * - It is suitable for precise event logging in systems like FSO gateways
 *   where timing around link events can matter.
 *
 * Why use localtime_r():
 * - localtime() returns a pointer to static internal storage and is not
 *   safe when called concurrently from multiple threads.
 * - localtime_r() writes into caller-provided storage, avoiding shared state.
 *
 * @param buffer      Destination buffer for the timestamp string.
 * @param buffer_size Size of the destination buffer in bytes.
 *
 * @return 0 on success, -1 on failure.
 */
static int format_timestamp(char *buffer, size_t buffer_size)
{
    struct timespec ts;
    struct tm tm_info;
    int written;

    if (buffer == NULL || buffer_size == 0U) {
        return -1;
    }

    /*
     * CLOCK_REALTIME gives us the current calendar time, which is appropriate
     * for human-readable logs. The tv_nsec field provides nanosecond precision.
     */
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return -1;
    }

    /*
     * Convert epoch seconds into local broken-down time in a thread-safe way.
     * localtime_r() avoids the shared static storage problem of localtime().
     */
    if (localtime_r(&ts.tv_sec, &tm_info) == NULL) {
        return -1;
    }

    /*
     * Produce the final timestamp in the exact required format:
     * YYYY-MM-DD HH:MM:SS.ns
     *
     * We print nanoseconds as a 9-digit zero-padded field.
     */
    written = snprintf(
        buffer,
        buffer_size,
        "%04d-%02d-%02d %02d:%02d:%02d.%09ld",
        tm_info.tm_year + 1900,
        tm_info.tm_mon + 1,
        tm_info.tm_mday,
        tm_info.tm_hour,
        tm_info.tm_min,
        tm_info.tm_sec,
        ts.tv_nsec
    );

    if (written < 0 || (size_t)written >= buffer_size) {
        return -1;
    }

    return 0;
}

/**
 * @brief Initialize the logging subsystem.
 *
 * This function initializes the global mutex used to serialize logging calls.
 * It is intended to be invoked once during program startup before any worker
 * threads begin emitting messages.
 *
 * @return 0 on success, -1 on failure.
 */
int log_init(void)
{
    if (g_log_initialized) {
        return 0;
    }

    if (pthread_mutex_init(&g_log_mutex, NULL) != 0) {
        return -1;
    }

    g_log_initialized = 1;
    g_log_min_level = DEBUG;
    return 0;
}

/**
 * @brief Shut down the logging subsystem.
 *
 * This destroys the mutex created by log_init(). It should only be called
 * after all logging activity has finished. Destroying a mutex that is still
 * in use by other threads is invalid, so orderly shutdown sequencing matters.
 *
 * @return 0 on success, -1 on failure.
 */
int log_shutdown(void)
{
    if (!g_log_initialized) {
        return 0;
    }

    if (pthread_mutex_destroy(&g_log_mutex) != 0) {
        return -1;
    }

    g_log_initialized = 0;
    return 0;
}

/**
 * @brief Set the minimum severity level that will be emitted.
 *
 * @param level The new threshold.
 *
 * @return 0 on success, -1 on failure.
 */
int log_set_level(log_level level)
{
    if (level < DEBUG || level > ERROR) {
        return -1;
    }

    g_log_min_level = level;
    return 0;
}

/**
 * @brief Get the current minimum severity threshold.
 *
 * @return The active threshold.
 */
log_level log_get_level(void)
{
    return g_log_min_level;
}

/**
 * @brief Emit a single formatted log line in a thread-safe manner.
 *
 * Internal steps:
 * 1. Validate logger initialization and arguments.
 * 2. Generate a human-readable high-resolution timestamp.
 * 3. Resolve the current thread ID.
 * 4. Lock the mutex so the full log line is emitted atomically.
 * 5. Print prefix and formatted message.
 * 6. Flush stdout so logs are visible immediately.
 * 7. Unlock the mutex.
 *
 * Thread-safety reasoning:
 * The mutex is locked around the entire output path rather than around only
 * individual fprintf() calls. This ensures that no other thread can insert
 * output in the middle of the current log line.
 *
 * @param level The severity level of the message.
 * @param fmt   A printf-style format string.
 * @param ...   Additional arguments matching the format string.
 *
 * @return 0 on success, -1 on failure.
 */
int log_msg(log_level level, const char *fmt, ...)
{
    char timestamp[64];
    const char *level_str;
    uintptr_t thread_id;
    va_list args;

    if (!g_log_initialized || fmt == NULL) {
        return -1;
    }

    if (level < g_log_min_level) {
        return 0;
    }

    if (format_timestamp(timestamp, sizeof(timestamp)) != 0) {
        return -1;
    }

    level_str = log_level_to_string(level);

    /*
     * pthread_t is an opaque type and is not guaranteed to be an integer.
     * Casting through uintptr_t gives us a practical printable value for
     * debugging/logging purposes on common platforms.
     */
    thread_id = (uintptr_t)pthread_self();

    /*
     * Lock before writing any part of the log line.
     * This prevents overlapping output from concurrent threads.
     */
    if (pthread_mutex_lock(&g_log_mutex) != 0) {
        return -1;
    }

    /*
     * Print the fixed prefix first.
     */
    fprintf(stdout, "[%s] [%s] [%lu] ", timestamp, level_str, (unsigned long)thread_id);

    /*
     * Print the caller's formatted message body.
     */
    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);

    /*
     * Terminate the log line and flush immediately.
     * Flushing helps during debugging and when the process crashes shortly
     * after logging an important event.
     */
    fputc('\n', stdout);
    fflush(stdout);

    /*
     * Unlock after the complete line has been emitted.
     */
    if (pthread_mutex_unlock(&g_log_mutex) != 0) {
        return -1;
    }

    return 0;
}