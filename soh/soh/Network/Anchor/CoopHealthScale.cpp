#include "soh/Network/Anchor/CoopHealthScale.h"
#include "soh/Network/Anchor/Anchor.h"
#include <libultraship/libultraship.h>
#include <cmath>
#include <unordered_map>

extern "C" {
#include "variables.h"
extern PlayState* gPlayState;
}

/**
 * See CoopHealthScale.h for the design.  The short version: we never touch an
 * actor's HP pool, we shrink each hit, so every native phase threshold keeps
 * working and only the number of hits needed grows.
 */

namespace {

// Fractional damage carried over between hits, per actor instance.
std::unordered_map<const Actor*, float> sCarry;
const PlayState* sCarryPlayState = nullptr;
s16 sCarryScene = -1;

// Mini-bosses that run as ACTORCAT_ENEMY in vanilla but play like a boss fight.
bool IsMiniboss(s16 actorId) {
    switch (actorId) {
        case ACTOR_EN_IK:       // Iron Knuckle
        case ACTOR_EN_TEST:     // Stalfos
        case ACTOR_EN_TORCH2:   // Dark Link
        case ACTOR_EN_FD:       // Flare Dancer
        case ACTOR_EN_BIGOKUTA: // Big Octo
        case ACTOR_EN_DH:       // Dead Hand
        case ACTOR_EN_ZF:       // Lizalfos / Dinolfos
            return true;
        default:
            return false;
    }
}

// Self + every online client that has a save loaded and stands in our scene and room.
int PlayersInMyRoom(const Anchor* anchor) {
    if (gPlayState == nullptr)
        return 1;
    const s16 myScene = gPlayState->sceneNum;
    const s8 myRoom = (s8)gPlayState->roomCtx.curRoom.num;
    int count = 1;
    for (const auto& [id, client] : anchor->clients) {
        if (client.self || !client.online || !client.isSaveLoaded)
            continue;
        if (client.sceneNum == myScene && client.curRoomNum == myRoom)
            count++;
    }
    return count;
}

void ResetIfSceneChanged() {
    if (gPlayState != sCarryPlayState || (gPlayState && gPlayState->sceneNum != sCarryScene)) {
        sCarry.clear();
        sCarryPlayState = gPlayState;
        sCarryScene = gPlayState ? gPlayState->sceneNum : -1;
    }
}

} // namespace

extern "C" int Anchor_ScaleIncomingDamage(struct Actor* actor, int damage) {
    if (actor == nullptr || damage <= 0)
        return damage;

    Anchor* anchor = Anchor::Instance;
    if (anchor == nullptr || !anchor->isConnected || !anchor->IsSaveLoaded())
        return damage;

    const RoomState& rs = anchor->roomState;
    // Scaling only makes sense when everyone fights the same shared enemy.
    if (!rs.syncEnemies || rs.battleRoyaleMode)
        return damage;

    u16 bonusPercent;
    if (actor->category == ACTORCAT_BOSS || (rs.minibossesAsBosses && IsMiniboss(actor->id))) {
        bonusPercent = rs.bossHealthBonus;
    } else if (actor->category == ACTORCAT_ENEMY) {
        bonusPercent = rs.enemyHealthBonus;
    } else {
        return damage; // NPCs, props, other players: never scaled
    }
    if (bonusPercent == 0)
        return damage;

    const int players = PlayersInMyRoom(anchor);
    if (players <= 1)
        return damage;

    const float hpMultiplier = 1.0f + (bonusPercent / 100.0f) * (float)(players - 1);
    if (hpMultiplier <= 1.0f)
        return damage;

    ResetIfSceneChanged();
    float& carry = sCarry[actor];
    const float exact = (float)damage / hpMultiplier + carry;
    // Small epsilon so 0.5 + 0.5 reliably lands on 1 instead of 0.99999.
    const int applied = (int)std::floor(exact + 0.0001f);
    carry = exact - (float)applied;
    if (carry < 0.0f)
        carry = 0.0f;

    SPDLOG_DEBUG("[Anchor:HealthScale] actor=0x{:04X} players={} mult={:.2f} damage {} -> {} (carry {:.2f})", actor->id,
                 players, hpMultiplier, damage, applied, carry);
    return applied;
}

extern "C" void Anchor_ResetDamageScaling(void) {
    sCarry.clear();
    sCarryPlayState = nullptr;
    sCarryScene = -1;
}
