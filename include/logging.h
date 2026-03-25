#ifndef LOGGING_H
#define LOGGING_H

/**
 * @file logging.h
 * @brief Public interface for the FSO Gateway logging module.
 *
 * This module provides a simple, thread-safe logging API for the project.
 * It supports four log levels and formats each message with:
 *
 *   [YYYY-MM-DD HH:MM:SS.ns] [LEVEL] [Thread_ID] message
 *
 * The implementation is designed to be safe when multiple threads log
 * concurrently, such as independent FSO transmit/receive directions.
 */

#include <stdarg.h>

/**
 * @enum log_level
 * @brief Enumerates the supported log severity levels.
 *
 * These values are used by log_msg() to categorize log messages.
 */
typedef enum log_level {
    DEBUG, /**< Verbose diagnostic information for development/troubleshooting */
    INFO,  /**< General operational information */
    WARN,  /**< Warning conditions that do not stop execution */
    ERROR  /**< Error conditions that may affect functionality */
} log_level;

/**
 * @brief Initialize the logging subsystem.
 *
 * This function must be called before any calls to log_msg() or the LOG_*
 * convenience macros. Internally, it initializes the mutex used to ensure
 * that concurrent log writes from multiple threads do not interleave.
 *
 * The function is safe to call once during program startup.
 *
 * @return 0 on success, -1 on failure.
 */
int log_init(void);

/**
 * @brief Shut down the logging subsystem.
 *
 * This function releases internal resources used by the logger, most notably
 * the mutex protecting concurrent writes.
 *
 * It should be called once during orderly program termination, after all
 * threads that may log have already stopped or have been joined.
 *
 * @return 0 on success, -1 on failure.
 */
int log_shutdown(void);

/**
 * @brief Set the minimum log level that will be emitted.
 *
 * Messages with a severity below this threshold are suppressed.
 * For example, setting INFO will suppress DEBUG messages while keeping
 * INFO/WARN/ERROR visible.
 *
 * @param level The minimum severity that should be printed.
 *
 * @return 0 on success, -1 on failure.
 */
int log_set_level(log_level level);

/**
 * @brief Get the current minimum log level threshold.
 *
 * @return The currently active minimum log level.
 */
log_level log_get_level(void);

/**
 * @brief Emit a formatted log message in a thread-safe manner.
 *
 * This function creates a timestamp, resolves the current thread ID,
 * formats the user-provided message, and writes the final log line to
 * standard output.
 *
 * The write path is protected by a mutex so that if multiple threads call
 * log_msg() at the same time, their output remains serialized and readable.
 *
 * @param level The severity level of the message.
 * @param fmt   A printf-style format string.
 * @param ...   Additional arguments corresponding to the format string.
 *
 * @return 0 on success, -1 on failure.
 */
int log_msg(log_level level, const char *fmt, ...);

/**
 * @brief Convenience macro for DEBUG-level logging.
 */
#define LOG_DEBUG(...) log_msg(DEBUG, __VA_ARGS__)

/**
 * @brief Convenience macro for INFO-level logging.
 */
#define LOG_INFO(...)  log_msg(INFO, __VA_ARGS__)

/**
 * @brief Convenience macro for WARN-level logging.
 */
#define LOG_WARN(...)  log_msg(WARN, __VA_ARGS__)

/**
 * @brief Convenience macro for ERROR-level logging.
 */
#define LOG_ERROR(...) log_msg(ERROR, __VA_ARGS__)

#endif /* LOGGING_H */