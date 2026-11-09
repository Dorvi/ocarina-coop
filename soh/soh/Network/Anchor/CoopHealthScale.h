#ifndef NETWORK_ANCHOR_COOP_HEALTH_SCALE_H
#define NETWORK_ANCHOR_COOP_HEALTH_SCALE_H

/**
 * Co-op health scaling
 *
 * Makes bosses (and optionally regular enemies) tougher when several players
 * fight them together.  Instead of raising an actor's HP — which would break
 * every boss whose phases are keyed to fixed HP thresholds (Ganon's knockdown
 * below 21, Volvagia's "never die from a non-killing hit", Phantom Ganon's
 * can-kill rule, ...) — we scale the *damage* each hit deals.
 *
 *   effective HP = native HP * (1 + bonus% / 100 * (playersInScene - 1))
 *
 * Fractions are carried per actor, so with 2x HP every second sword hit
 * removes 1 HP instead of every hit; the total number of hits needed is
 * exactly the scaled amount, and all native thresholds still trigger at the
 * same HP values they always did.
 *
 * Only active while connected to an Anchor room with Enemy Sync ON and
 * Battle Royale OFF.  Everything is a no-op in single player.
 */

#ifdef __cplusplus
extern "C" {
#endif

struct Actor;

/**
 * Returns the damage that should actually be subtracted from `actor`'s HP
 * for a hit of `damage`.  Call it right where the game subtracts health.
 * `damage` <= 0 is returned unchanged.
 */
int Anchor_ScaleIncomingDamage(struct Actor* actor, int damage);

/** Drops all carried-over fractional damage (called on scene change). */
void Anchor_ResetDamageScaling(void);

#ifdef __cplusplus
}
#endif

#endif // NETWORK_ANCHOR_COOP_HEALTH_SCALE_H
