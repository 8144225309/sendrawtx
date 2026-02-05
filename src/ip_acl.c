#include "ip_acl.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <arpa/inet.h>

/*
 * Parse IP string to 16-byte address (IPv4-mapped IPv6 format).
 * Returns 0 on success, -1 on error.
 */
static int parse_ip_to_addr(const char *ip_str, uint8_t *addr)
{
    struct in_addr addr4;
    struct in6_addr addr6;

    memset(addr, 0, 16);

    /* Try IPv4 first */
    if (inet_pton(AF_INET, ip_str, &addr4) == 1) {
        /* Store as IPv4-mapped IPv6: ::ffff:x.x.x.x */
        addr[10] = 0xff;
        addr[11] = 0xff;
        memcpy(&addr[12], &addr4, 4);
        return 0;
    }

    /* Try IPv6 */
    if (inet_pton(AF_INET6, ip_str, &addr6) == 1) {
        memcpy(addr, &addr6, 16);
        return 0;
    }

    return -1;
}

/*
 * Parse CIDR notation (e.g., "192.168.0.0/16" or "2001:db8::/32").
 * Returns 0 on success, -1 on error.
 * Output: addr (16 bytes), prefix_len (0-128)
 */
static int parse_cidr(const char *cidr_str, uint8_t *addr, uint8_t *prefix_len)
{
    char ip_part[INET6_ADDRSTRLEN];
    const char *slash;
    int prefix;
    int is_ipv4;

    /* Find the slash */
    slash = strchr(cidr_str, '/');
    if (!slash) {
        return -1;
    }

    /* Extract IP part */
    size_t ip_len = slash - cidr_str;
    if (ip_len >= sizeof(ip_part)) {
        return -1;
    }
    memcpy(ip_part, cidr_str, ip_len);
    ip_part[ip_len] = '\0';

    /* Parse prefix length */
    prefix = atoi(slash + 1);

    /* Determine if IPv4 or IPv6 and parse address */
    struct in_addr addr4;
    struct in6_addr addr6;

    memset(addr, 0, 16);

    if (inet_pton(AF_INET, ip_part, &addr4) == 1) {
        /* IPv4 - convert prefix to IPv4-mapped IPv6 range */
        is_ipv4 = 1;
        if (prefix < 0 || prefix > 32) {
            return -1;
        }
        /* Store as IPv4-mapped IPv6 */
        addr[10] = 0xff;
        addr[11] = 0xff;
        memcpy(&addr[12], &addr4, 4);
        /* Convert IPv4 prefix to IPv6 prefix: /N -> /96+N */
        *prefix_len = 96 + prefix;
    } else if (inet_pton(AF_INET6, ip_part, &addr6) == 1) {
        /* IPv6 */
        is_ipv4 = 0;
        if (prefix < 0 || prefix > 128) {
            return -1;
        }
        memcpy(addr, &addr6, 16);
        *prefix_len = prefix;
    } else {
        return -1;
    }

    (void)is_ipv4;  /* Suppress unused warning */
    return 0;
}

/*
 * FNV-1a hash function for 16-byte address.
 * Same algorithm as rate_limiter.c for consistency.
 */
static uint32_t hash_addr(const uint8_t *addr)
{
    uint32_t hash = 2166136261u;
    for (int i = 0; i < 16; i++) {
        hash ^= addr[i];
        hash *= 16777619u;
    }
    return hash;
}

/*
 * Compare two addresses for equality.
 */
static int addrs_equal(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 16) == 0;
}

/*
 * Check if addr matches CIDR range (network/prefix_len).
 * Returns 1 if match, 0 if no match.
 */
static int cidr_match(const uint8_t *addr, const uint8_t *network, uint8_t prefix_len)
{
    int full_bytes = prefix_len / 8;
    int remaining_bits = prefix_len % 8;

    /* Compare full bytes */
    if (full_bytes > 0 && memcmp(addr, network, full_bytes) != 0) {
        return 0;
    }

    /* Compare remaining bits if any */
    if (remaining_bits > 0 && full_bytes < 16) {
        uint8_t mask = (0xff << (8 - remaining_bits)) & 0xff;
        if ((addr[full_bytes] & mask) != (network[full_bytes] & mask)) {
            return 0;
        }
    }

    return 1;
}

/*
 * Trim whitespace from string (in place).
 */
static char *trim(char *str)
{
    char *end;

    /* Trim leading space */
    while (isspace((unsigned char)*str)) str++;

    if (*str == 0) {
        return str;
    }

    /* Trim trailing space */
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;

    end[1] = '\0';
    return str;
}

int ip_acl_init(IpACL *acl)
{
    memset(acl, 0, sizeof(*acl));

    acl->exact_buckets = calloc(IP_ACL_HASH_SIZE, sizeof(ACLEntry *));
    if (!acl->exact_buckets) {
        return -1;
    }

    acl->num_exact_buckets = IP_ACL_HASH_SIZE;
    acl->num_exact_entries = 0;
    acl->cidr_entries = NULL;
    acl->num_cidr_entries = 0;
    acl->source_file[0] = '\0';

    return 0;
}

void ip_acl_free(IpACL *acl)
{
    if (!acl) return;

    /* Free exact entries */
    if (acl->exact_buckets) {
        for (int i = 0; i < acl->num_exact_buckets; i++) {
            ACLEntry *entry = acl->exact_buckets[i];
            while (entry) {
                ACLEntry *next = entry->next;
                free(entry);
                entry = next;
            }
        }
        free(acl->exact_buckets);
        acl->exact_buckets = NULL;
    }

    /* Free CIDR entries */
    ACLEntry *cidr = acl->cidr_entries;
    while (cidr) {
        ACLEntry *next = cidr->next;
        free(cidr);
        cidr = next;
    }
    acl->cidr_entries = NULL;

    acl->num_exact_entries = 0;
    acl->num_cidr_entries = 0;
}

/*
 * Add an exact IP entry to the ACL.
 */
static int add_exact_entry(IpACL *acl, const uint8_t *addr)
{
    uint32_t hash = hash_addr(addr);
    int bucket = hash % acl->num_exact_buckets;

    /* Check if already exists */
    ACLEntry *entry = acl->exact_buckets[bucket];
    while (entry) {
        if (addrs_equal(entry->addr, addr)) {
            return 0;  /* Already exists */
        }
        entry = entry->next;
    }

    /* Create new entry */
    entry = malloc(sizeof(ACLEntry));
    if (!entry) {
        return -1;
    }

    memcpy(entry->addr, addr, 16);
    entry->prefix_len = 128;  /* Exact match */
    entry->next = acl->exact_buckets[bucket];
    acl->exact_buckets[bucket] = entry;
    acl->num_exact_entries++;

    return 0;
}

/*
 * Add a CIDR range entry to the ACL.
 */
static int add_cidr_entry(IpACL *acl, const uint8_t *addr, uint8_t prefix_len)
{
    /* Check if already exists */
    ACLEntry *entry = acl->cidr_entries;
    while (entry) {
        if (addrs_equal(entry->addr, addr) && entry->prefix_len == prefix_len) {
            return 0;  /* Already exists */
        }
        entry = entry->next;
    }

    /* Create new entry */
    entry = malloc(sizeof(ACLEntry));
    if (!entry) {
        return -1;
    }

    memcpy(entry->addr, addr, 16);
    entry->prefix_len = prefix_len;
    entry->next = acl->cidr_entries;
    acl->cidr_entries = entry;
    acl->num_cidr_entries++;

    return 0;
}

int ip_acl_load_file(IpACL *acl, const char *path)
{
    FILE *f;
    char line[512];
    int count = 0;
    int line_num = 0;

    if (!path || !path[0]) {
        return 0;  /* Empty path = no file to load */
    }

    f = fopen(path, "r");
    if (!f) {
        log_warn("Cannot open ACL file '%s': %s", path, strerror(errno));
        return -1;
    }

    /* Store source file path */
    strncpy(acl->source_file, path, sizeof(acl->source_file) - 1);
    acl->source_file[sizeof(acl->source_file) - 1] = '\0';

    while (fgets(line, sizeof(line), f)) {
        line_num++;
        char *trimmed = trim(line);

        /* Skip empty lines and comments */
        if (*trimmed == '\0' || *trimmed == '#') {
            continue;
        }

        uint8_t addr[16];
        uint8_t prefix_len;

        /* Check if CIDR notation */
        if (strchr(trimmed, '/')) {
            if (parse_cidr(trimmed, addr, &prefix_len) < 0) {
                log_warn("Invalid CIDR entry at %s:%d: %s", path, line_num, trimmed);
                continue;
            }
            if (add_cidr_entry(acl, addr, prefix_len) < 0) {
                log_error("Failed to add CIDR entry: %s", trimmed);
                continue;
            }
        } else {
            /* Exact IP */
            if (parse_ip_to_addr(trimmed, addr) < 0) {
                log_warn("Invalid IP at %s:%d: %s", path, line_num, trimmed);
                continue;
            }
            if (add_exact_entry(acl, addr) < 0) {
                log_error("Failed to add exact IP entry: %s", trimmed);
                continue;
            }
        }

        count++;
    }

    fclose(f);

    log_info("Loaded %d ACL entries from %s (%d exact, %d CIDR)",
             count, path, acl->num_exact_entries, acl->num_cidr_entries);

    return count;
}

int ip_acl_contains(IpACL *acl, const char *ip_str)
{
    uint8_t addr[16];

    if (!acl || !acl->exact_buckets) {
        return 0;  /* ACL not initialized */
    }

    if (parse_ip_to_addr(ip_str, addr) < 0) {
        return 0;  /* Can't parse IP - fail open */
    }

    /* Check exact match first (O(1) average) */
    uint32_t hash = hash_addr(addr);
    int bucket = hash % acl->num_exact_buckets;
    ACLEntry *entry = acl->exact_buckets[bucket];
    while (entry) {
        if (addrs_equal(entry->addr, addr)) {
            return 1;  /* Found exact match */
        }
        entry = entry->next;
    }

    /* Check CIDR ranges (O(n) where n = number of CIDR entries) */
    entry = acl->cidr_entries;
    while (entry) {
        if (cidr_match(addr, entry->addr, entry->prefix_len)) {
            return 1;  /* Found CIDR match */
        }
        entry = entry->next;
    }

    return 0;  /* Not found */
}

int ip_acl_context_init(IpACLContext *ctx)
{
    if (ip_acl_init(&ctx->blocklist) < 0) {
        return -1;
    }

    if (ip_acl_init(&ctx->allowlist) < 0) {
        ip_acl_free(&ctx->blocklist);
        return -1;
    }

    return 0;
}

void ip_acl_context_free(IpACLContext *ctx)
{
    if (!ctx) return;

    ip_acl_free(&ctx->blocklist);
    ip_acl_free(&ctx->allowlist);
}

IpACLResult ip_acl_check(IpACLContext *ctx, const char *ip_str)
{
    if (!ctx) {
        return IP_ACL_NEUTRAL;
    }

    /* Check blocklist first (blocklist takes precedence) */
    if (ip_acl_contains(&ctx->blocklist, ip_str)) {
        return IP_ACL_BLOCK;
    }

    /* Check allowlist */
    if (ip_acl_contains(&ctx->allowlist, ip_str)) {
        return IP_ACL_ALLOW;
    }

    return IP_ACL_NEUTRAL;
}

const char *ip_acl_stats(IpACL *acl)
{
    static char buf[256];

    if (!acl) {
        snprintf(buf, sizeof(buf), "disabled");
        return buf;
    }

    snprintf(buf, sizeof(buf), "%d exact + %d CIDR entries",
             acl->num_exact_entries, acl->num_cidr_entries);
    return buf;
}
