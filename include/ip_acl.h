#ifndef IP_ACL_H
#define IP_ACL_H

#include <stdint.h>

/*
 * IP Access Control List (ACL) for blocklist/allowlist functionality.
 * Supports IPv4, IPv6, and CIDR notation with hot-reload via SIGHUP.
 *
 * Design:
 * - Exact IPs stored in hash table for O(1) lookup
 * - CIDR ranges stored in linked list (scanned on each check)
 * - IPv4 addresses stored as IPv4-mapped IPv6 (::ffff:x.x.x.x)
 */

/* Hash table size for exact IP lookups */
#define IP_ACL_HASH_SIZE 1024

/* ACL entry - represents either exact IP or CIDR range */
typedef struct ACLEntry {
    uint8_t addr[16];          /* IPv6 or IPv4-mapped IPv6 address */
    uint8_t prefix_len;        /* CIDR prefix: 0-128 for IPv6, 96-128 for IPv4-mapped */
    struct ACLEntry *next;     /* Hash chain (exact) or linked list (CIDR) */
} ACLEntry;

/* IP ACL structure - holds either blocklist or allowlist */
typedef struct IpACL {
    ACLEntry **exact_buckets;  /* Hash table for exact IP lookups */
    int num_exact_buckets;
    int num_exact_entries;
    ACLEntry *cidr_entries;    /* Linked list for CIDR ranges */
    int num_cidr_entries;
    char source_file[256];     /* Path to source file for logging */
} IpACL;

/* Combined ACL context with both blocklist and allowlist */
typedef struct IpACLContext {
    IpACL blocklist;
    IpACL allowlist;
} IpACLContext;

/* Result of IP ACL check */
typedef enum {
    IP_ACL_ALLOW,      /* IP is in allowlist - bypass rate limiting */
    IP_ACL_BLOCK,      /* IP is in blocklist - reject connection */
    IP_ACL_NEUTRAL     /* IP not in either list - apply normal rules */
} IpACLResult;

/*
 * Initialize an empty ACL.
 * Returns 0 on success, -1 on allocation failure.
 */
int ip_acl_init(IpACL *acl);

/*
 * Free all resources associated with an ACL.
 */
void ip_acl_free(IpACL *acl);

/*
 * Load ACL entries from a file.
 * File format: one entry per line, supports:
 * - IPv4: 192.168.1.1
 * - IPv6: 2001:db8::1
 * - CIDR: 192.168.0.0/16, 2001:db8::/32
 * - Comments: lines starting with #
 * - Empty lines are ignored
 *
 * Returns number of entries loaded, or -1 on error.
 */
int ip_acl_load_file(IpACL *acl, const char *path);

/*
 * Check if an IP address is in the ACL.
 * Checks exact match first, then CIDR ranges.
 * Returns 1 if found, 0 if not found.
 */
int ip_acl_contains(IpACL *acl, const char *ip_str);

/*
 * Initialize ACL context (both blocklist and allowlist).
 * Returns 0 on success, -1 on allocation failure.
 */
int ip_acl_context_init(IpACLContext *ctx);

/*
 * Free ACL context resources.
 */
void ip_acl_context_free(IpACLContext *ctx);

/*
 * Check IP against both blocklist and allowlist.
 * Order: blocklist checked first, then allowlist.
 *
 * Returns:
 * - IP_ACL_BLOCK if IP is in blocklist
 * - IP_ACL_ALLOW if IP is in allowlist (and not in blocklist)
 * - IP_ACL_NEUTRAL if IP is in neither list
 */
IpACLResult ip_acl_check(IpACLContext *ctx, const char *ip_str);

/*
 * Get statistics string for logging.
 * Returns pointer to static buffer (not thread-safe).
 */
const char *ip_acl_stats(IpACL *acl);

#endif /* IP_ACL_H */
