#ifndef NETWORK_ANCHOR_COOP_BOSS_H
#define NETWORK_ANCHOR_COOP_BOSS_H

/**
 * Co-op bosses (shared boss health)
 *
 * Every player still runs their own copy of the boss, but teammates standing in
 * the same boss room share one health pool:
 *
 *  - Health is synced downwards: whenever a teammate's boss has less HP than
 *    ours, ours drops to match (never below 1, so the native death logic is
 *    only ever triggered by a real hit).
 *  - When one player lands the killing blow, the others get the room reloaded
 *    with the boss already cleared (blue warp + heart container), using the
 *    game's own "boss already defeated" path.
 *  - Each hit is scaled down by the number of teammates in the room, so the
 *    fight lasts longer with more players. HP itself is never raised, so every
 *    native HP threshold (phase changes, "sword only" finishers) still works.
 *
 * Everything here is a no-op unless connected to an Anchor room with
 * "Shared Bosses" enabled.
 */

#ifdef __cplusplus
extern "C" {
#endif

struct Actor;

/**
 * Returns the damage that should actually be subtracted from a boss's HP for
 * a hit of `damage`. Call it right where the game subtracts boss health.
 * Non-boss actors and `damage` <= 0 are returned unchanged.
 */
int CoopBoss_ScaleDamage(struct Actor* actor, int damage);

#ifdef __cplusplus
}
#endif

#endif // NETWORK_ANCHOR_COOP_BOSS_H
