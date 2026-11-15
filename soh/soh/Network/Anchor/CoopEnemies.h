#ifndef NETWORK_ANCHOR_COOP_ENEMIES_H
#define NETWORK_ANCHOR_COOP_ENEMIES_H

/**
 * Co-op enemies (shared enemy deaths)
 *
 * Every player still runs their own copy of each enemy, but when a teammate in
 * the same room kills an enemy, that enemy dies on everyone's screen at the
 * same moment, with its normal death animation.
 *
 * How: the killing hit is replayed. The killer sends which enemy died (by its
 * position in the room's actor list, which is identical for everyone) and what
 * hit it (damage type, which collider). Receivers mark that hit on their own
 * copy of the enemy, so the enemy's own damage code runs and plays its death.
 * If an enemy refuses the hit for 3 seconds (it is in a state where it cannot
 * be damaged), it is removed with the usual defeat flames instead.
 *
 * Only enemies placed by the room are shared. Enemies generated at runtime
 * (Stalchildren in Hyrule Field, Leevers, ...) have no stable identity across
 * players and stay local.
 *
 * Everything here is a no-op unless connected to an Anchor room with
 * "Shared Enemies" enabled.
 */

#ifdef __cplusplus
extern "C" {
#endif

struct PlayState;

/**
 * Called once per frame right after CollisionCheck_Damage and before the
 * collision context is cleared, i.e. when this frame's hits are known and
 * actors have not reacted to them yet.
 */
void CoopEnemies_AfterCollisionCheck(struct PlayState* play);

#ifdef __cplusplus
}
#endif

#endif // NETWORK_ANCHOR_COOP_ENEMIES_H
