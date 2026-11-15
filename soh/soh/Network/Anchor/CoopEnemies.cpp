#include "soh/Network/Anchor/CoopEnemies.h"
#include "soh/Network/Anchor/Anchor.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ObjectExtension/ActorListIndex.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include <algorithm>
#include <list>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "macros.h"
extern PlayState* gPlayState;
}

/**
 * See CoopEnemies.h for the design.
 *
 * ENEMY_DEFEATED { sceneNum, roomNum, listIndex, actorId, colliderOrdinal, element, dmgFlags, effect, damage }
 *
 * Sent to teammates in the same scene when a room-placed enemy dies locally.
 */

namespace {

// How long (in game frames, 20/s) we keep replaying the killing hit before giving up.
constexpr int kReplayFrames = 60;
// How many frames after a hit we still attribute a death to it.
constexpr int kRecentHitFrames = 4;

struct HitInfo {
    int colliderOrdinal = -1; // which of the actor's AC colliders was hit, in registration order
    int element = 0;          // which element of that collider
    u32 dmgFlags = DMG_SLASH_MASTER;
    u8 effect = 0;
    u8 damage = 1;
};

struct RecentHit {
    HitInfo hit;
    u8 healthBefore = 0;
    int age = 0;
};

struct DoomedEnemy {
    Actor* actor = nullptr;
    HitInfo hit;
    u8 healthAtStart = 0;
    int frames = 0;
};

// Local hits from the last few frames, to know what killed an enemy.
std::unordered_map<Actor*, RecentHit> sRecentHits;
// Enemies a teammate killed that are still alive here.
std::list<DoomedEnemy> sDoomed;
// Enemies whose death was already reported or received: never report them (again).
std::unordered_set<Actor*> sSettled;

// The fake attacker handed to an enemy's damage code. Enemies may keep pointers to these
// after we are done with them, so they live in static storage and are reused round-robin.
constexpr size_t kFakeHitPool = 64;
Collider sFakeAt[kFakeHitPool];
ColliderInfo sFakeAtInfo[kFakeHitPool];
size_t sFakeHitNext = 0;

bool IsActive(Anchor* anchor) {
    return anchor != nullptr && anchor->isConnected && anchor->IsSaveLoaded() && gPlayState != nullptr &&
           anchor->roomState.sharedEnemies;
}

std::string OwnTeamId() {
    return CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
}

bool IsTeammateInMyScene(const AnchorClient& client, const std::string& teamId) {
    return !client.self && client.online && client.isSaveLoaded && client.teamId == teamId &&
           client.sceneNum == gPlayState->sceneNum;
}

// Room-placed enemies only: their index in the room's actor list identifies them for everyone.
bool IsSharedEnemy(const Actor* actor) {
    return actor != nullptr && actor->category == ACTORCAT_ENEMY && GetActorListIndex(actor) >= 0;
}

// Calls fn(info, elementIndex) for every element of a collider.
template <typename Fn> void ForEachElement(Collider* collider, Fn fn) {
    switch (collider->shape) {
        case COLSHAPE_JNTSPH: {
            ColliderJntSph* jntSph = (ColliderJntSph*)collider;
            if (jntSph->elements != nullptr) {
                for (s32 i = 0; i < jntSph->count; i++) {
                    fn(&jntSph->elements[i].info, i);
                }
            }
            break;
        }
        case COLSHAPE_CYLINDER:
            fn(&((ColliderCylinder*)collider)->info, 0);
            break;
        case COLSHAPE_TRIS: {
            ColliderTris* tris = (ColliderTris*)collider;
            if (tris->elements != nullptr) {
                for (s32 i = 0; i < tris->count; i++) {
                    fn(&tris->elements[i].info, i);
                }
            }
            break;
        }
        case COLSHAPE_QUAD:
            fn(&((ColliderQuad*)collider)->info, 0);
            break;
        default:
            break;
    }
}

Actor* FindSharedEnemy(s8 roomNum, s16 listIndex, s16 actorId) {
    for (Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_ENEMY].head; actor != nullptr; actor = actor->next) {
        if (actor->update != nullptr && actor->id == actorId && actor->room == roomNum &&
            GetActorListIndex(actor) == listIndex) {
            return actor;
        }
    }
    return nullptr;
}

void ReportLocalDeath(Anchor* anchor, Actor* actor) {
    if (!sSettled.insert(actor).second) {
        return; // already reported, or it died because a teammate killed it
    }

    HitInfo hit;
    if (auto it = sRecentHits.find(actor); it != sRecentHits.end()) {
        hit = it->second.hit;
    }
    anchor->SendPacket_EnemyDefeated(actor->room, GetActorListIndex(actor), actor->id, hit.colliderOrdinal, hit.element,
                                     hit.dmgFlags, hit.effect, hit.damage);
}

// Step 1: enemies hit a moment ago whose health reached zero died from that hit.
void DetectLocalDeaths(Anchor* anchor) {
    for (auto it = sRecentHits.begin(); it != sRecentHits.end();) {
        Actor* actor = it->first;
        RecentHit& recent = it->second;

        if (recent.healthBefore > 0 && actor->colChkInfo.health == 0) {
            ReportLocalDeath(anchor, actor);
            it = sRecentHits.erase(it);
        } else if (++recent.age > kRecentHitFrames) {
            it = sRecentHits.erase(it);
        } else {
            ++it;
        }
    }
}

// Step 2: remember which collider/element of each shared enemy was hit this frame, and by what.
void RecordLocalHits(PlayState* play) {
    CollisionCheckContext* colChkCtx = &play->colChkCtx;

    for (s32 i = 0; i < colChkCtx->colACCount; i++) {
        Collider* collider = colChkCtx->colAC[i];
        if (collider == nullptr || !(collider->acFlags & AC_HIT) || !IsSharedEnemy(collider->actor)) {
            continue;
        }
        Actor* actor = collider->actor;
        if (sSettled.count(actor)) {
            continue;
        }

        int ordinal = 0;
        for (s32 j = 0; j < i; j++) {
            if (colChkCtx->colAC[j] != nullptr && colChkCtx->colAC[j]->actor == actor) {
                ordinal++;
            }
        }

        ForEachElement(collider, [&](ColliderInfo* info, int element) {
            if ((info->bumperFlags & BUMP_HIT) && info->acHitInfo != nullptr) {
                RecentHit recent;
                recent.hit.colliderOrdinal = ordinal;
                recent.hit.element = element;
                recent.hit.dmgFlags = info->acHitInfo->toucher.dmgFlags;
                recent.hit.effect = info->acHitInfo->toucher.effect;
                recent.hit.damage = info->acHitInfo->toucher.damage;
                recent.healthBefore = actor->colChkInfo.health;
                sRecentHits[actor] = recent;
            }
        });
    }
}

// Marks `hit` on the enemy's own collider, exactly as the collision system would, so that the
// enemy's own damage code reacts to it on its next update. Returns false if the enemy has no
// collider that can be hit this frame.
bool ReplayHit(PlayState* play, DoomedEnemy& doomed) {
    CollisionCheckContext* colChkCtx = &play->colChkCtx;
    Actor* actor = doomed.actor;

    std::vector<Collider*> colliders;
    for (s32 i = 0; i < colChkCtx->colACCount; i++) {
        Collider* collider = colChkCtx->colAC[i];
        if (collider != nullptr && collider->actor == actor && (collider->acFlags & AC_ON)) {
            colliders.push_back(collider);
        }
    }
    if (colliders.empty()) {
        return false;
    }

    // Prefer the same collider and element the killer hit; otherwise the first one that takes damage.
    Collider* target = nullptr;
    ColliderInfo* targetInfo = nullptr;
    auto pick = [&](Collider* collider, int wantedElement) {
        ForEachElement(collider, [&](ColliderInfo* info, int element) {
            if (targetInfo == nullptr && (wantedElement < 0 || element == wantedElement) &&
                (info->bumperFlags & BUMP_ON) && !(info->bumperFlags & BUMP_NO_DAMAGE)) {
                target = collider;
                targetInfo = info;
            }
        });
    };
    if (doomed.hit.colliderOrdinal >= 0 && doomed.hit.colliderOrdinal < (int)colliders.size()) {
        pick(colliders[doomed.hit.colliderOrdinal], doomed.hit.element);
    }
    for (size_t i = 0; i < colliders.size() && targetInfo == nullptr; i++) {
        if (!(colliders[i]->acFlags & AC_HARD)) {
            pick(colliders[i], -1);
        }
    }
    if (targetInfo == nullptr) {
        return false;
    }

    Actor* attacker = &GET_PLAYER(play)->actor;
    size_t slot = sFakeHitNext++ % kFakeHitPool;
    Collider* fakeAt = &sFakeAt[slot];
    ColliderInfo* fakeAtInfo = &sFakeAtInfo[slot];
    *fakeAt = {};
    *fakeAtInfo = {};
    fakeAt->actor = attacker;
    fakeAt->atFlags = AT_ON | AT_TYPE_PLAYER;
    fakeAtInfo->toucher.dmgFlags = doomed.hit.dmgFlags;
    fakeAtInfo->toucher.effect = doomed.hit.effect;
    fakeAtInfo->toucher.damage = doomed.hit.damage;
    fakeAtInfo->toucherFlags = TOUCH_ON;

    // Same bookkeeping as CollisionCheck_SetATvsAC
    target->acFlags |= AC_HIT;
    target->ac = attacker;
    targetInfo->acHit = fakeAt;
    targetInfo->acHitInfo = fakeAtInfo;
    targetInfo->bumperFlags |= BUMP_HIT;
    targetInfo->bumper.hitPos.x = (s16)actor->focus.pos.x;
    targetInfo->bumper.hitPos.y = (s16)actor->focus.pos.y;
    targetInfo->bumper.hitPos.z = (s16)actor->focus.pos.z;
    actor->colChkInfo.acHitEffect = doomed.hit.effect;

    // Same as CollisionCheck_ApplyDamage, except the damage is always enough to kill
    u8 damage = std::max<u8>(doomed.hit.damage, 1);
    if (DamageTable* tbl = actor->colChkInfo.damageTable; tbl != nullptr) {
        s32 i;
        u32 flags = doomed.hit.dmgFlags;
        for (i = 0; i < 0x20; i++, flags >>= 1) {
            if (flags == 1) {
                break;
            }
        }
        if (i < 0x20) {
            damage = std::max<u8>(tbl->table[i] & 0xF, 1);
            actor->colChkInfo.damageEffect = (tbl->table[i] >> 4) & 0xF;
        }
    }
    actor->colChkInfo.damage = std::max(damage, actor->colChkInfo.health);

    return true;
}

// The enemy never accepted the hit: remove it with the usual defeat flames.
void RemoveWithFlames(PlayState* play, Actor* actor) {
    static Vec3f sZeroVec = { 0.0f, 0.0f, 0.0f };
    Vec3f pos = actor->focus.pos;

    EffectSsDeadDb_Spawn(play, &pos, &sZeroVec, &sZeroVec, 100, 0, 255, 255, 255, 255, 0, 0, 255, 1, 9, true);
    Audio_PlayActorSound2(actor, NA_SE_EN_EXTINCT);
    Actor_Kill(actor);
}

// Step 3: keep replaying the killing hit on enemies a teammate killed, until they die.
void ReplayRemoteDeaths(PlayState* play) {
    for (auto it = sDoomed.begin(); it != sDoomed.end();) {
        DoomedEnemy& doomed = *it;
        Actor* actor = doomed.actor;

        bool dead = actor->update == nullptr || (doomed.healthAtStart > 0 && actor->colChkInfo.health == 0);
        if (dead) {
            it = sDoomed.erase(it);
            continue;
        }
        if (++doomed.frames > kReplayFrames) {
            RemoveWithFlames(play, actor);
            it = sDoomed.erase(it);
            continue;
        }

        ReplayHit(play, doomed);
        ++it;
    }
}

void Forget(Actor* actor) {
    sRecentHits.erase(actor);
    sSettled.erase(actor);
    sDoomed.remove_if([actor](const DoomedEnemy& doomed) { return doomed.actor == actor; });
}

void ForgetAll() {
    sRecentHits.clear();
    sSettled.clear();
    sDoomed.clear();
}

} // namespace

extern "C" void CoopEnemies_AfterCollisionCheck(PlayState* play) {
    Anchor* anchor = Anchor::Instance;
    if (!IsActive(anchor) || play != gPlayState) {
        if (!sDoomed.empty() || !sRecentHits.empty()) {
            ForgetAll();
        }
        return;
    }

    DetectLocalDeaths(anchor);
    RecordLocalHits(play);
    ReplayRemoteDeaths(play);
}

// MARK: - Packets

void Anchor::SendPacket_EnemyDefeated(s8 roomNum, s16 listIndex, s16 actorId, s32 colliderOrdinal, s32 element,
                                      u32 dmgFlags, u8 effect, u8 damage) {
    if (!IsSaveLoaded()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = ENEMY_DEFEATED;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["roomNum"] = roomNum;
    payload["listIndex"] = listIndex;
    payload["actorId"] = actorId;
    payload["colliderOrdinal"] = colliderOrdinal;
    payload["element"] = element;
    payload["dmgFlags"] = dmgFlags;
    payload["effect"] = effect;
    payload["damage"] = damage;
    payload["quiet"] = true;

    const std::string teamId = OwnTeamId();
    for (auto& [clientId, client] : clients) {
        if (IsTeammateInMyScene(client, teamId)) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

void Anchor::HandlePacket_EnemyDefeated(nlohmann::json payload) {
    if (!IsActive(this)) {
        return;
    }

    uint32_t clientId = payload.at("clientId").get<uint32_t>();
    if (!clients.contains(clientId) || clients[clientId].teamId != OwnTeamId()) {
        return;
    }
    if (payload.at("sceneNum").get<s16>() != gPlayState->sceneNum) {
        return;
    }

    Actor* actor = FindSharedEnemy(payload.at("roomNum").get<s8>(), payload.at("listIndex").get<s16>(),
                                   payload.at("actorId").get<s16>());
    if (actor == nullptr) {
        return; // not loaded here, or a different enemy sits in that slot (e.g. enemy randomizer)
    }
    if (!sSettled.insert(actor).second) {
        return; // already dead or dying here
    }

    DoomedEnemy doomed;
    doomed.actor = actor;
    doomed.healthAtStart = actor->colChkInfo.health;
    doomed.hit.colliderOrdinal = payload.at("colliderOrdinal").get<s32>();
    doomed.hit.element = payload.at("element").get<s32>();
    doomed.hit.dmgFlags = payload.at("dmgFlags").get<u32>();
    doomed.hit.effect = payload.at("effect").get<u8>();
    doomed.hit.damage = payload.at("damage").get<u8>();
    sDoomed.push_back(doomed);
}

// MARK: - Hooks

void Anchor::RegisterCoopEnemiesHooks() {
    COND_HOOK(OnSceneInit, isConnected, [&](int16_t sceneNum) { ForgetAll(); });

    // Actor memory is reused, so never keep a pointer to an actor that is gone.
    COND_HOOK(OnActorDestroy, isConnected, [&](void* refActor) { Forget((Actor*)refActor); });

    // Enemies that die without their health reaching zero from a hit (falls, scripted deaths, ...).
    COND_HOOK(OnEnemyDefeat, isConnected, [&](void* refActor) {
        Actor* actor = (Actor*)refActor;
        if (IsActive(this) && IsSharedEnemy(actor)) {
            ReportLocalDeath(this, actor);
        }
    });
}
