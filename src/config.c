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
 *   --internal-symbol-crc 0
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
 */
static void config_set_defaults(struct config *cfg)
{
    if (cfg == NULL) {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));

    strncpy(cfg->lan_iface, "eth0", sizeof(cfg->lan_iface) - 1);
    strncpy(cfg->fso_iface, "eth1", sizeof(cfg->fso_iface) - 1);

    cfg->k           = 10;
    cfg->m           = 4;
    cfg->depth       = 8;
    cfg->symbol_size = 16;

    /*
     * Internal per-symbol CRC is ENABLED by default.
     * Set --internal-symbol-crc 0 to disable for throughput experiments.
     */
    cfg->internal_symbol_crc_enabled = 1;
}

/**
 * @brief Parse a non-negative integer from a string.
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

    memcpy(dst, src, src_len + 1U);
    return 0;
}

/**
 * @brief Parse command-line arguments into a configuration structure.
 */
int config_parse(int argc, char *argv[], struct config *cfg)
{
    int opt;
    int option_index = 0;

    enum {
        OPT_LAN_IFACE = 1000,
        OPT_FSO_IFACE,
        OPT_K,
        OPT_M,
        OPT_DEPTH,
        OPT_SYMBOL_SIZE,
        OPT_INTERNAL_SYMBOL_CRC
    };

    static const struct option long_options[] = {
        {"lan-iface",            required_argument, NULL, OPT_LAN_IFACE},
        {"fso-iface",            required_argument, NULL, OPT_FSO_IFACE},
        {"k",                    required_argument, NULL, OPT_K},
        {"m",                    required_argument, NULL, OPT_M},
        {"depth",                required_argument, NULL, OPT_DEPTH},
        {"symbol-size",          required_argument, NULL, OPT_SYMBOL_SIZE},
        {"internal-symbol-crc",  required_argument, NULL, OPT_INTERNAL_SYMBOL_CRC},
        {0,                      0,                 0,    0}
    };

    if (cfg == NULL) {
        LOG_ERROR("config_parse received NULL cfg pointer");
        return -1;
    }

    config_set_defaults(cfg);

    optind = 1;

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

            case OPT_INTERNAL_SYMBOL_CRC: {
                int val = 0;
                if (parse_non_negative_int(optarg, &val, "internal_symbol_crc") != 0) {
                    return -1;
                }
                /* Accept 0 or 1 only */
                if (val != 0 && val != 1) {
                    LOG_ERROR("Invalid value for internal_symbol_crc: %d (must be 0 or 1)", val);
                    return -1;
                }
                cfg->internal_symbol_crc_enabled = val;
                break;
            }

            case '?':
                LOG_ERROR("Failed to parse command-line arguments");
                return -1;

            default:
                LOG_ERROR("Unexpected option encountered during parsing");
                return -1;
        }
    }

    if (optind < argc) {
        LOG_ERROR("Unexpected positional argument: '%s'", argv[optind]);
        return -1;
    }

    return 0;
}
