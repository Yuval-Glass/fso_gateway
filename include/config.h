#ifndef CONFIG_H
#define CONFIG_H

/**
 * @file config.h
 * @brief Runtime configuration parsing interface for the FSO Gateway.
 *
 * This header defines the configuration structure used by the application
 * and declares the function responsible for parsing command-line arguments.
 *
 * The configuration is populated from command-line options using getopt_long()
 * in the corresponding implementation file.
 */

#include <stddef.h>

/**
 * @struct config
 * @brief Holds all runtime configuration parameters for the FSO Gateway.
 *
 * This structure contains:
 * - Network interface names for the LAN side and FSO side
 * - FEC-related parameters
 * - Processing/interleaving-related parameters
 *
 * All fields are populated by config_parse().
 */
struct config {
    /**
     * @brief Name of the LAN-facing network interface.
     *
     * Example: "eth0"
     */
    char lan_iface[32];

    /**
     * @brief Name of the FSO-facing network interface.
     *
     * Example: "eth1"
     */
    char fso_iface[32];

    /**
     * @brief FEC source/data block size parameter.
     *
     * This value typically represents the number of original data units
     * in an FEC block.
     */
    int k;

    /**
     * @brief FEC redundancy/parity block size parameter.
     *
     * This value typically represents the number of redundant units
     * added for forward error correction.
     */
    int m;

    /**
     * @brief Processing depth parameter.
     *
     * This may represent interleaving depth, buffering depth,
     * or other pipeline-related processing depth depending on
     * later module design.
     */
    int depth;

    /**
     * @brief Processing symbol size parameter.
     *
     * This may represent the size of a processed symbol in bytes
     * or another unit defined by later FEC/interleaving logic.
     */
    int symbol_size;
};

/**
 * @brief Parse command-line arguments into a configuration structure.
 *
 * This function:
 * 1. Initializes the provided configuration structure with defaults.
 * 2. Iterates over argc/argv using getopt_long().
 * 3. Recognizes long-form options such as:
 *    --lan-iface, --fso-iface, --k, --m, --depth, --symbol-size
 * 4. Validates parsed values.
 * 5. Returns success or failure.
 *
 * Expected example usage:
 * @code
 * struct config cfg;
 * if (config_parse(argc, argv, &cfg) != 0) {
 *     return 1;
 * }
 * @endcode
 *
 * @param argc Argument count passed to main().
 * @param argv Argument vector passed to main().
 * @param cfg Pointer to a configuration structure to populate.
 *
 * @return 0 on success, -1 on failure.
 */
int config_parse(int argc, char *argv[], struct config *cfg);

#endif /* CONFIG_H */