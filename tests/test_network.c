/*
 * Network/Chain Detection Test - Phase 13e
 *
 * Tests address prefix detection for mixed mode routing.
 */

#include <stdio.h>
#include <string.h>
#include "network.h"

static int tests = 0;
static int passed = 0;

#define TEST(addr, expected_chain) do { \
    tests++; \
    int detected = network_detect_chain_from_address(addr); \
    if (detected == expected_chain) { \
        passed++; \
        printf("  PASS: %s -> %s\n", addr, network_chain_to_string(expected_chain)); \
    } else { \
        printf("  FAIL: %s -> expected %s, got %s\n", addr, \
               network_chain_to_string(expected_chain), \
               detected >= 0 ? network_chain_to_string(detected) : "INVALID"); \
    } \
} while(0)

int main(void)
{
    printf("================================================\n");
    printf("Network Detection Test Suite - Phase 13e\n");
    printf("================================================\n");

    printf("\n--- Mainnet Addresses ---\n");
    TEST("bc1qar0srrr7xfkvy5l643lydnw9re59gtzzwf5mdq", CHAIN_MAINNET);
    TEST("bc1p5cyxnuxmeuwuvkwfem96lqzszd02n6xdcjrs20cac6yqjjwudpxqkedrcr", CHAIN_MAINNET);
    TEST("1BvBMSEYstWetqTFn5Au4m4GFg7xJaNVN2", CHAIN_MAINNET);
    TEST("3J98t1WpEZ73CNmQviecrnyiWrnqRhWNLy", CHAIN_MAINNET);

    printf("\n--- Testnet/Signet Addresses ---\n");
    TEST("tb1qw508d6qejxtdg4y5r3zarvary0c5xw7kxpjzsx", CHAIN_TESTNET);
    TEST("tb1p5cyxnuxmeuwuvkwfem96lqzszd02n6xdcjrs20cac6yqjjwudpxqp3pjtt", CHAIN_TESTNET);
    TEST("mipcBbFg9gMiCh81Kj8tqqdgoZub1ZJRfn", CHAIN_TESTNET);
    TEST("n1wgm6kkzMcNfAtJmes8YhpvtDzdNhDY5a", CHAIN_TESTNET);
    TEST("2MzQwSSnBHWHqSAqtTVQ6v47XtaisrJa1Vc", CHAIN_TESTNET);

    printf("\n--- Regtest Addresses ---\n");
    TEST("bcrt1qs758ursh4q9z627kt3pp5yysm78ddny6txaqgw", CHAIN_REGTEST);
    TEST("bcrt1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqc8gma6", CHAIN_REGTEST);

    printf("\n--- Invalid Addresses ---\n");
    tests++;
    int result = network_detect_chain_from_address("invalid");
    if (result < 0) {
        passed++;
        printf("  PASS: 'invalid' -> INVALID\n");
    } else {
        printf("  FAIL: 'invalid' should be INVALID, got %s\n", network_chain_to_string(result));
    }

    tests++;
    result = network_detect_chain_from_address("");
    if (result < 0) {
        passed++;
        printf("  PASS: '' (empty) -> INVALID\n");
    } else {
        printf("  FAIL: empty should be INVALID, got %s\n", network_chain_to_string(result));
    }

    printf("\n--- Chain String Conversion ---\n");
    tests++;
    if (network_chain_from_string("mixed") == CHAIN_MIXED) {
        passed++;
        printf("  PASS: 'mixed' -> CHAIN_MIXED\n");
    } else {
        printf("  FAIL: 'mixed' should parse to CHAIN_MIXED\n");
    }

    tests++;
    if (strcmp(network_chain_to_string(CHAIN_MIXED), "mixed") == 0) {
        passed++;
        printf("  PASS: CHAIN_MIXED -> 'mixed'\n");
    } else {
        printf("  FAIL: CHAIN_MIXED should stringify to 'mixed'\n");
    }

    printf("\n--- Mixed Mode Banner Logic ---\n");
    tests++;
    if (!network_is_test_network(CHAIN_MIXED)) {
        passed++;
        printf("  PASS: CHAIN_MIXED is not a test network (no banner)\n");
    } else {
        printf("  FAIL: CHAIN_MIXED should not be a test network\n");
    }

    tests++;
    if (network_get_banner_text(CHAIN_MIXED) == NULL) {
        passed++;
        printf("  PASS: CHAIN_MIXED has no banner text\n");
    } else {
        printf("  FAIL: CHAIN_MIXED should have no banner\n");
    }

    printf("\n================================================\n");
    printf("Results: %d/%d tests passed\n", passed, tests);
    printf("================================================\n");

    return (passed == tests) ? 0 : 1;
}
