/*
 * Bitcoin Network/Chain Mode - Phase 13
 *
 * Handles chain identification, address validation, and network banners.
 */

#include "network.h"
#include <string.h>
#include <ctype.h>

/*
 * Address prefix patterns for each network:
 *
 * Mainnet:
 *   - P2PKH: 1... (base58, starts with 1)
 *   - P2SH:  3... (base58, starts with 3)
 *   - Bech32: bc1q... (native segwit v0)
 *   - Bech32m: bc1p... (taproot, segwit v1)
 *
 * Testnet (testnet3):
 *   - P2PKH: m... or n... (base58)
 *   - P2SH:  2... (base58)
 *   - Bech32: tb1q... (native segwit v0)
 *   - Bech32m: tb1p... (taproot)
 *
 * Signet:
 *   - Same prefixes as testnet (tb1...)
 *
 * Regtest:
 *   - P2PKH: m... or n... (base58, same as testnet)
 *   - P2SH:  2... (base58, same as testnet)
 *   - Bech32: bcrt1q... (native segwit v0)
 *   - Bech32m: bcrt1p... (taproot)
 */

int network_chain_from_string(const char *name)
{
    if (!name) return -1;

    /* Case-insensitive comparison */
    if (strcasecmp(name, "mainnet") == 0 || strcasecmp(name, "main") == 0) {
        return CHAIN_MAINNET;
    }
    if (strcasecmp(name, "testnet") == 0 || strcasecmp(name, "testnet3") == 0 ||
        strcasecmp(name, "test") == 0) {
        return CHAIN_TESTNET;
    }
    if (strcasecmp(name, "signet") == 0) {
        return CHAIN_SIGNET;
    }
    if (strcasecmp(name, "regtest") == 0 || strcasecmp(name, "reg") == 0) {
        return CHAIN_REGTEST;
    }
    if (strcasecmp(name, "mixed") == 0 || strcasecmp(name, "multi") == 0 ||
        strcasecmp(name, "multichain") == 0) {
        return CHAIN_MIXED;
    }

    return -1;
}

const char *network_chain_to_string(BitcoinChain chain)
{
    switch (chain) {
        case CHAIN_MAINNET: return "mainnet";
        case CHAIN_TESTNET: return "testnet";
        case CHAIN_SIGNET:  return "signet";
        case CHAIN_REGTEST: return "regtest";
        case CHAIN_MIXED:   return "mixed";
        default:            return "unknown";
    }
}

const char *network_get_header_value(BitcoinChain chain)
{
    /* Same as chain_to_string for now, but separate function
     * in case we want different formatting later */
    return network_chain_to_string(chain);
}

/*
 * Detect which network an address belongs to based on prefix.
 * Returns the detected chain, or -1 if unrecognized.
 */
static int detect_address_network(const char *address)
{
    if (!address || strlen(address) < 2) {
        return -1;
    }

    /* Bech32/Bech32m addresses - check HRP (human-readable part) */
    if (strncmp(address, "bc1", 3) == 0) {
        /* Mainnet bech32: bc1q... (segwit v0) or bc1p... (taproot) */
        return CHAIN_MAINNET;
    }
    if (strncmp(address, "tb1", 3) == 0) {
        /* Testnet/Signet bech32: tb1q... or tb1p... */
        /* Can't distinguish testnet from signet by address alone */
        return CHAIN_TESTNET;
    }
    if (strncmp(address, "bcrt1", 4) == 0) {
        /* Regtest bech32: bcrt1q... or bcrt1p... */
        return CHAIN_REGTEST;
    }

    /* Base58 addresses - check first character */
    char first = address[0];

    if (first == '1') {
        /* Mainnet P2PKH */
        return CHAIN_MAINNET;
    }
    if (first == '3') {
        /* Mainnet P2SH */
        return CHAIN_MAINNET;
    }
    if (first == 'm' || first == 'n') {
        /* Testnet/Regtest P2PKH - can't distinguish without more context */
        /* Default to testnet, but regtest uses same prefixes */
        return CHAIN_TESTNET;
    }
    if (first == '2') {
        /* Testnet/Regtest P2SH */
        return CHAIN_TESTNET;
    }

    return -1;
}

AddressCheckResult network_check_address(BitcoinChain expected,
                                         const char *address,
                                         BitcoinChain *detected_network)
{
    int detected = detect_address_network(address);

    if (detected < 0) {
        return ADDR_INVALID;
    }

    if (detected_network) {
        *detected_network = (BitcoinChain)detected;
    }

    /* Special cases for ambiguous prefixes */

    /* Regtest uses same base58 prefixes as testnet (m/n/2) */
    if (expected == CHAIN_REGTEST && detected == CHAIN_TESTNET) {
        /* Check if it's a bech32 address - those are distinguishable */
        if (strncmp(address, "tb1", 3) == 0) {
            /* Definitely testnet, not regtest */
            return ADDR_WRONG_NETWORK;
        }
        /* Base58 address - could be either, allow it */
        return ADDR_MATCH;
    }

    /* Signet uses same prefixes as testnet */
    if (expected == CHAIN_SIGNET && detected == CHAIN_TESTNET) {
        return ADDR_MATCH;
    }
    if (expected == CHAIN_TESTNET && detected == CHAIN_TESTNET) {
        /* Could be signet address on testnet or vice versa - allow */
        return ADDR_MATCH;
    }

    /* Direct match */
    if ((int)expected == detected) {
        return ADDR_MATCH;
    }

    return ADDR_WRONG_NETWORK;
}

const char *network_get_address_warning(BitcoinChain server_chain,
                                        BitcoinChain address_chain)
{
    /* Static buffers for warning messages */
    static const char *warnings[4][4] = {
        /* Server: MAINNET */
        {
            NULL,  /* mainnet addr on mainnet - no warning */
            "Warning: This appears to be a TESTNET address. Server is running on MAINNET.",
            "Warning: This appears to be a SIGNET address. Server is running on MAINNET.",
            "Warning: This appears to be a REGTEST address. Server is running on MAINNET."
        },
        /* Server: TESTNET */
        {
            "Warning: This appears to be a MAINNET address. Server is running on TESTNET.",
            NULL,  /* testnet addr on testnet - no warning */
            NULL,  /* signet addr on testnet - can't distinguish */
            "Warning: This appears to be a REGTEST address. Server is running on TESTNET."
        },
        /* Server: SIGNET */
        {
            "Warning: This appears to be a MAINNET address. Server is running on SIGNET.",
            NULL,  /* testnet addr on signet - can't distinguish */
            NULL,  /* signet addr on signet - no warning */
            "Warning: This appears to be a REGTEST address. Server is running on SIGNET."
        },
        /* Server: REGTEST */
        {
            "Warning: This appears to be a MAINNET address. Server is running on REGTEST.",
            "Warning: This appears to be a TESTNET address. Server is running on REGTEST.",
            "Warning: This appears to be a SIGNET address. Server is running on REGTEST.",
            NULL   /* regtest addr on regtest - no warning */
        }
    };

    if (server_chain > CHAIN_REGTEST || address_chain > CHAIN_REGTEST) {
        return "Warning: Unknown network mismatch detected.";
    }

    return warnings[server_chain][address_chain];
}

int network_is_test_network(BitcoinChain chain)
{
    /* Mixed mode is not considered a test network (no banner on homepage) */
    return chain != CHAIN_MAINNET && chain != CHAIN_MIXED;
}

const char *network_get_banner_text(BitcoinChain chain)
{
    switch (chain) {
        case CHAIN_MAINNET:
        case CHAIN_MIXED:
            return NULL;  /* No banner for mainnet or mixed mode homepage */
        case CHAIN_TESTNET:
            return "TESTNET - Coins have no value";
        case CHAIN_SIGNET:
            return "SIGNET - Coins have no value";
        case CHAIN_REGTEST:
            return "REGTEST - Local test network";
        default:
            return "UNKNOWN NETWORK";
    }
}

const char *network_get_banner_class(BitcoinChain chain)
{
    switch (chain) {
        case CHAIN_MAINNET:
        case CHAIN_MIXED:
            return NULL;  /* No banner class for mainnet or mixed */
        case CHAIN_TESTNET:
            return "network-banner-testnet";
        case CHAIN_SIGNET:
            return "network-banner-signet";
        case CHAIN_REGTEST:
            return "network-banner-regtest";
        default:
            return "network-banner-unknown";
    }
}

int network_detect_chain_from_address(const char *address)
{
    return detect_address_network(address);
}

int network_detect_chain_from_address_with_hint(const char *address, BitcoinChain hint)
{
    int detected = detect_address_network(address);

    if (detected < 0) {
        return -1;  /* Invalid address */
    }

    /* If detected as testnet and hint is signet, use signet */
    /* (tb1 addresses are ambiguous between testnet and signet) */
    if (detected == CHAIN_TESTNET && hint == CHAIN_SIGNET) {
        /* Check if it's a tb1 address (ambiguous) */
        if (address && strncmp(address, "tb1", 3) == 0) {
            return CHAIN_SIGNET;
        }
        /* m/n/2 addresses are also used by signet */
        if (address && (address[0] == 'm' || address[0] == 'n' || address[0] == '2')) {
            return CHAIN_SIGNET;
        }
    }

    return detected;
}
