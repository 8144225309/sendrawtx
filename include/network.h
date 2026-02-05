#ifndef NETWORK_H
#define NETWORK_H

/*
 * Bitcoin Network/Chain Mode - Phase 13
 *
 * Explicit chain specification to prevent mixing networks.
 * Server must start with exactly one chain configured.
 *
 * Features:
 * - X-Bitcoin-Network HTTP header on all responses
 * - Address prefix validation with warnings
 * - Visual banners for non-mainnet modes
 */

/*
 * Supported Bitcoin networks.
 * Each has distinct address prefixes and genesis blocks.
 */
typedef enum {
    CHAIN_MAINNET,   /* Production network */
    CHAIN_TESTNET,   /* Public test network (testnet3) */
    CHAIN_SIGNET,    /* Signed test network */
    CHAIN_REGTEST,   /* Local regression test network */
    CHAIN_MIXED      /* Multi-chain mode - routes by address detection */
} BitcoinChain;

/*
 * Address type detection result.
 */
typedef enum {
    ADDR_MATCH,           /* Address matches configured chain */
    ADDR_WRONG_NETWORK,   /* Address is for a different network */
    ADDR_INVALID          /* Not a valid Bitcoin address format */
} AddressCheckResult;

/*
 * Get the chain enum from a string name.
 * Returns -1 if invalid.
 */
int network_chain_from_string(const char *name);

/*
 * Get the string name for a chain.
 */
const char *network_chain_to_string(BitcoinChain chain);

/*
 * Get the HTTP header value for the network.
 * Returns: "mainnet", "testnet", "signet", "regtest"
 */
const char *network_get_header_value(BitcoinChain chain);

/*
 * Check if an address matches the configured network.
 * Returns ADDR_MATCH, ADDR_WRONG_NETWORK, or ADDR_INVALID.
 *
 * If wrong_network is non-NULL and result is ADDR_WRONG_NETWORK,
 * it will be set to the detected network of the address.
 */
AddressCheckResult network_check_address(BitcoinChain expected,
                                         const char *address,
                                         BitcoinChain *detected_network);

/*
 * Get a human-readable warning message for address mismatch.
 * Returns static string, do not free.
 * Example: "Warning: This appears to be a mainnet address but server is running in regtest mode"
 */
const char *network_get_address_warning(BitcoinChain server_chain,
                                        BitcoinChain address_chain);

/*
 * Check if the chain is a test network (not mainnet).
 * Used to determine if warnings/banners should be shown.
 */
int network_is_test_network(BitcoinChain chain);

/*
 * Get banner text for test networks.
 * Returns NULL for mainnet, warning text for others.
 */
const char *network_get_banner_text(BitcoinChain chain);

/*
 * Get banner CSS class for test networks.
 * Returns NULL for mainnet, class name for others.
 */
const char *network_get_banner_class(BitcoinChain chain);

/*
 * Detect chain from a Bitcoin address.
 * Used in mixed mode to route transactions.
 * Returns the detected chain, or -1 if unrecognized.
 *
 * Note: tb1 addresses are ambiguous between testnet and signet.
 * This function returns CHAIN_TESTNET for tb1 addresses.
 * Use network_detect_chain_from_address_with_hint for disambiguation.
 */
int network_detect_chain_from_address(const char *address);

/*
 * Detect chain from address with a hint for ambiguous cases.
 * If the address is ambiguous (tb1), the hint is used.
 * hint should be CHAIN_TESTNET or CHAIN_SIGNET.
 */
int network_detect_chain_from_address_with_hint(const char *address, BitcoinChain hint);

#endif /* NETWORK_H */
