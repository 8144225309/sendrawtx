#include "slot_manager.h"

void slot_manager_init(SlotManager *sm, int normal_max, int large_max, int huge_max)
{
    sm->normal_current = 0;
    sm->normal_max = normal_max;
    sm->large_current = 0;
    sm->large_max = large_max;
    sm->huge_current = 0;
    sm->huge_max = huge_max;
}

int slot_manager_acquire(SlotManager *sm, RequestTier tier)
{
    switch (tier) {
        case TIER_NORMAL:
            if (sm->normal_current >= sm->normal_max) {
                return 0;
            }
            sm->normal_current++;
            return 1;

        case TIER_LARGE:
            if (sm->large_current >= sm->large_max) {
                return 0;
            }
            sm->large_current++;
            return 1;

        case TIER_HUGE:
            if (sm->huge_current >= sm->huge_max) {
                return 0;
            }
            sm->huge_current++;
            return 1;

        default:
            return 0;
    }
}

void slot_manager_release(SlotManager *sm, RequestTier tier)
{
    switch (tier) {
        case TIER_NORMAL:
            if (sm->normal_current > 0) {
                sm->normal_current--;
            }
            break;

        case TIER_LARGE:
            if (sm->large_current > 0) {
                sm->large_current--;
            }
            break;

        case TIER_HUGE:
            if (sm->huge_current > 0) {
                sm->huge_current--;
            }
            break;

        default:
            break;
    }
}

int slot_manager_promote(SlotManager *sm, RequestTier from_tier, RequestTier to_tier)
{
    /* Same tier - nothing to do */
    if (from_tier == to_tier) {
        return 1;
    }

    /* Can't demote (only promote) */
    if (to_tier < from_tier) {
        return 0;
    }

    /* Try to acquire new tier first */
    if (!slot_manager_acquire(sm, to_tier)) {
        return 0;  /* No slots in new tier */
    }

    /* Release old tier */
    slot_manager_release(sm, from_tier);

    return 1;
}

int slot_manager_current(SlotManager *sm, RequestTier tier)
{
    switch (tier) {
        case TIER_NORMAL: return sm->normal_current;
        case TIER_LARGE:  return sm->large_current;
        case TIER_HUGE:   return sm->huge_current;
        default:          return 0;
    }
}

int slot_manager_max(SlotManager *sm, RequestTier tier)
{
    switch (tier) {
        case TIER_NORMAL: return sm->normal_max;
        case TIER_LARGE:  return sm->large_max;
        case TIER_HUGE:   return sm->huge_max;
        default:          return 0;
    }
}

/* UNHOOKED: Not called from production or test code. */
int slot_manager_total_connections(SlotManager *sm)
{
    return sm->normal_current + sm->large_current + sm->huge_current;
}

/*
 * UNHOOKED: Not called from production or test code.
 * Written for a planned per-request (vs per-connection) slot tracking model
 * that was never adopted. Keep-alive code in connection.c uses
 * slot_manager_acquire()/slot_manager_release() directly instead.
 */
int slot_manager_acquire_request(SlotManager *sm, RequestTier tier)
{
    return slot_manager_acquire(sm, tier);
}

/* UNHOOKED: See slot_manager_acquire_request() note above. */
void slot_manager_release_request(SlotManager *sm, RequestTier tier)
{
    slot_manager_release(sm, tier);
}
