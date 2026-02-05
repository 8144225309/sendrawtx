#ifndef SLOT_MANAGER_H
#define SLOT_MANAGER_H

#include "reader.h"  /* For RequestTier enum */

/*
 * SlotManager - Per-worker connection slot limits with tier support.
 *
 * Supports dynamic tier promotion for large URL handling:
 * - NORMAL: Default tier for new connections (e.g., 100 slots)
 * - LARGE:  For requests > large_threshold (e.g., 20 slots)
 * - HUGE:   For requests > huge_threshold (e.g., 5 slots)
 *
 * Prevents resource exhaustion while allowing large transactions.
 * Each worker has its own SlotManager (no locking needed).
 */

typedef struct SlotManager {
    int normal_current;
    int normal_max;
    int large_current;
    int large_max;
    int huge_current;
    int huge_max;
} SlotManager;

/*
 * Initialize slot manager with configured limits.
 */
void slot_manager_init(SlotManager *sm, int normal_max, int large_max, int huge_max);

/*
 * Acquire a slot at specified tier.
 * Returns 1 on success, 0 if no slots available.
 */
int slot_manager_acquire(SlotManager *sm, RequestTier tier);

/*
 * Release a slot at specified tier.
 */
void slot_manager_release(SlotManager *sm, RequestTier tier);

/*
 * Promote a connection from one tier to another.
 * Releases the old tier slot and acquires a new tier slot.
 * Returns 1 on success, 0 if new tier has no slots available.
 * On failure, the old tier slot is NOT released (connection keeps old tier).
 */
int slot_manager_promote(SlotManager *sm, RequestTier from_tier, RequestTier to_tier);

/*
 * Get current slot usage for a tier.
 */
int slot_manager_current(SlotManager *sm, RequestTier tier);

/*
 * Get max slots for a tier.
 */
int slot_manager_max(SlotManager *sm, RequestTier tier);

/*
 * Get total active connections across all tiers.
 */
int slot_manager_total_connections(SlotManager *sm);

/* Legacy functions for backward compatibility */
#define slot_manager_acquire_normal(sm) slot_manager_acquire((sm), TIER_NORMAL)
#define slot_manager_release_normal(sm) slot_manager_release((sm), TIER_NORMAL)

/*
 * Request slot accounting for keep-alive connections.
 * HTTP/1.1 keep-alive: slot tracks requests, not connections.
 * HTTP/2: slot tracks streams (handled in http2.c).
 */

/*
 * Called when starting a new request on a keep-alive connection.
 * For the first request, the slot is already held from accept.
 * For subsequent requests, we need to re-acquire.
 */
int slot_manager_acquire_request(SlotManager *sm, RequestTier tier);

/*
 * Called after completing a request on a keep-alive connection.
 * Releases the slot to allow other connections to use it.
 * The connection stays open but doesn't hold a slot until next request.
 */
void slot_manager_release_request(SlotManager *sm, RequestTier tier);

#endif /* SLOT_MANAGER_H */
