/**
 * @file config.c
 * @brief Command-line configuration parsing for the FSO Gateway.
 *
 * This module uses getopt_long() to parse long-form command-line options
 * and populate a struct config instance for the rest of the application.
 *
 * Example supported arguments:
 *   --lan-iface eth0
 *   --fso-iface eth1
 *   --k 10
 *   --m 4
 *   --depth 8
 *   --symbol-size 16
 *
 * Parsing flow:
 * - A default configuration is first installed into the provided struct.
 * - getopt_long() then iterates through argv and returns one option at a time.
 * - For each recognized option, the corresponding field in struct config
 *   is updated.
 * - Numeric fields are validated to ensure they are non-negative.
 * - On invalid input, an error is logged and parsing fails.
 */

#include "config.h"
#include "logging.h"

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Set default runtime configuration values.
 *
 * This helper initializes the configuration structure with safe,
 * predictable defaults so that omitted options still yield a valid
 * runtime configuration.
 *
 * Default values used here are placeholders and can be adjusted later
 * as the gateway processing pipeline becomes more defined.
 *
 * @param cfg Pointer to the configuration structure to initialize.
 */
static void config_set_defaults(struct config *cfg)
{
    if (cfg == NULL) {
        return;
    }

    /*
     * Zero the structure first so all bytes are initialized.
     * This prevents accidental use of uninitialized data.
     */
    memset(cfg, 0, sizeof(*cfg));

    /*
     * Default interface names.
     * These can be overridden at runtime with:
     *   --lan-iface <name>
     *   --fso-iface <name>
     */
    strncpy(cfg->lan_iface, "eth0", sizeof(cfg->lan_iface) - 1);
    strncpy(cfg->fso_iface, "eth1", sizeof(cfg->fso_iface) - 1);

    /*
     * Default FEC parameters.
     * These are sample defaults and may be refined later based on
     * the actual coding scheme used in the FSO Gateway.
     */
    cfg->k = 10;
    cfg->m = 4;

    /*
     * Default processing parameters.
     */
    cfg->depth = 8;
    cfg->symbol_size = 16;
}

/**
 * @brief Parse a non-negative integer from a string.
 *
 * This function converts a string into an int while performing
 * strict validation:
 * - The string must not be NULL or empty.
 * - The full string must represent a valid integer.
 * - The value must fit within the int range.
 * - The value must be zero or positive.
 *
 * This is used for parameters such as k, m, depth, and symbol_size.
 *
 * @param text Input string to parse.
 * @param out_value Output location for the parsed integer.
 * @param field_name Human-readable field name used in log messages.
 *
 * @return 0 on success, -1 on failure.
 */
static int parse_non_negative_int(const char *text, int *out_value, const char *field_name)
{
    long value;
    char *endptr = NULL;

    if (text == NULL || out_value == NULL || field_name == NULL) {
        LOG_ERROR("Internal config parsing error: NULL argument");
        return -1;
    }

    if (*text == '\0') {
        LOG_ERROR("Invalid value for %s: empty string", field_name);
        return -1;
    }

    errno = 0;
    value = strtol(text, &endptr, 10);

    /*
     * Validate conversion results:
     * - errno catches overflows/underflows
     * - endptr == text means no digits were parsed
     * - *endptr != '\0' means trailing invalid characters exist
     */
    if (errno != 0 || endptr == text || *endptr != '\0') {
        LOG_ERROR("Invalid numeric value for %s: '%s'", field_name, text);
        return -1;
    }

    if (value < 0) {
        LOG_ERROR("Invalid value for %s: negative values are not allowed (%ld)", field_name, value);
        return -1;
    }

    if (value > INT_MAX) {
        LOG_ERROR("Invalid value for %s: value too large (%ld)", field_name, value);
        return -1;
    }

    *out_value = (int)value;
    return 0;
}

/**
 * @brief Copy an interface name into a fixed-size destination buffer.
 *
 * This helper ensures that interface names fit into the destination array
 * and remain NUL-terminated.
 *
 * @param dst Destination character buffer.
 * @param dst_size Size of the destination buffer in bytes.
 * @param src Source string to copy.
 * @param field_name Human-readable field name used in log messages.
 *
 * @return 0 on success, -1 on failure.
 */
static int copy_string_option(char *dst, size_t dst_size, const char *src, const char *field_name)
{
    size_t src_len;

    if (dst == NULL || src == NULL || field_name == NULL || dst_size == 0U) {
        LOG_ERROR("Internal config parsing error while copying %s", field_name);
        return -1;
    }

    src_len = strlen(src);
    if (src_len >= dst_size) {
        LOG_ERROR("Value for %s is too long: '%s' (max %zu characters)", field_name, src, dst_size - 1U);
        return -1;
    }

    /*
     * Copy including the terminating NUL byte.
     */
    memcpy(dst, src, src_len + 1U);
    return 0;
}

/**
 * @brief Parse command-line arguments into a configuration structure.
 *
 * getopt_long() behavior overview:
 * - argc is the total number of command-line arguments.
 * - argv is the array of strings containing each argument.
 * - getopt_long() internally walks through argv from left to right.
 * - On each iteration it returns one recognized option.
 * - For options that require an argument, the option value is made available
 *   via the global pointer optarg.
 * - When no more options remain, getopt_long() returns -1 and the loop ends.
 *
 * Example:
 *   ./fso_gateway --k 10 --m 4 --lan-iface eth0
 *
 * Iteration sequence may look like:
 *   1. returns OPT_K, optarg = "10"
 *   2. returns OPT_M, optarg = "4"
 *   3. returns OPT_LAN_IFACE, optarg = "eth0"
 *   4. returns -1
 *
 * This function applies defaults first, then overrides those defaults
 * with any user-provided command-line arguments.
 *
 * @param argc Argument count from main().
 * @param argv Argument vector from main().
 * @param cfg Pointer to the configuration structure to fill.
 *
 * @return 0 on success, -1 on failure.
 */
int config_parse(int argc, char *argv[], struct config *cfg)
{
    int opt;
    int option_index = 0;

    /*
     * Enumerated values starting above the ASCII range so they do not
     * conflict with ordinary single-character option return values.
     */
    enum {
        OPT_LAN_IFACE = 1000,
        OPT_FSO_IFACE,
        OPT_K,
        OPT_M,
        OPT_DEPTH,
        OPT_SYMBOL_SIZE
    };

    /**
     * Long options table for getopt_long().
     *
     * Each entry maps a command-line option name to:
     * - whether it requires an argument
     * - what value getopt_long() should return when matched
     *
     * Example:
     *   --k 10
     * causes getopt_long() to return OPT_K and sets optarg = "10".
     */
    static const struct option long_options[] = {
        {"lan-iface",   required_argument, NULL, OPT_LAN_IFACE},
        {"fso-iface",   required_argument, NULL, OPT_FSO_IFACE},
        {"k",           required_argument, NULL, OPT_K},
        {"m",           required_argument, NULL, OPT_M},
        {"depth",       required_argument, NULL, OPT_DEPTH},
        {"symbol-size", required_argument, NULL, OPT_SYMBOL_SIZE},
        {0,             0,                 0,    0}
    };

    if (cfg == NULL) {
        LOG_ERROR("config_parse received NULL cfg pointer");
        return -1;
    }

    config_set_defaults(cfg);

    /*
     * Reset getopt state.
     * This is helpful if config_parse() is ever called more than once
     * in the same process, for example during tests.
     */
    optind = 1;

    /*
     * Iterate over all recognized command-line options.
     * getopt_long() returns:
     * - a defined option code when an option is found
     * - -1 when parsing is complete
     * - '?' when an unknown option or missing argument is encountered
     */
    while ((opt = getopt_long(argc, argv, "", long_options, &option_index)) != -1) {
        switch (opt) {
            case OPT_LAN_IFACE:
                if (copy_string_option(cfg->lan_iface,
                                       sizeof(cfg->lan_iface),
                                       optarg,
                                       "lan_iface") != 0) {
                    return -1;
                }
                break;

            case OPT_FSO_IFACE:
                if (copy_string_option(cfg->fso_iface,
                                       sizeof(cfg->fso_iface),
                                       optarg,
                                       "fso_iface") != 0) {
                    return -1;
                }
                break;

            case OPT_K:
                if (parse_non_negative_int(optarg, &cfg->k, "k") != 0) {
                    return -1;
                }
                break;

            case OPT_M:
                if (parse_non_negative_int(optarg, &cfg->m, "m") != 0) {
                    return -1;
                }
                break;

            case OPT_DEPTH:
                if (parse_non_negative_int(optarg, &cfg->depth, "depth") != 0) {
                    return -1;
                }
                break;

            case OPT_SYMBOL_SIZE:
                if (parse_non_negative_int(optarg, &cfg->symbol_size, "symbol_size") != 0) {
                    return -1;
                }
                break;

            case '?':
                /*
                 * getopt_long() already detected an invalid option or a missing
                 * required argument. We log a generic error and fail.
                 */
                LOG_ERROR("Failed to parse command-line arguments");
                return -1;

            default:
                LOG_ERROR("Unexpected option encountered during parsing");
                return -1;
        }
    }

    /*
     * If additional unexpected positional arguments remain after option parsing,
     * treat that as an error for now to keep the interface strict and predictable.
     */
    if (optind < argc) {
        LOG_ERROR("Unexpected positional argument: '%s'", argv[optind]);
        return -1;
    }

    return 0;
}