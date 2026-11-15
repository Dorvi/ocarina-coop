#include "soh/Network/Anchor/CoopBoss.h"
#include "soh/Network/Anchor/Anchor.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Notification/Notification.h"
#include <nlohmann/json.hpp>
#include <libultraship/libultraship.h>
#include <cmath>
#include <unordered_map>

extern "C" {
#include "variables.h"
#include "functions.h"
#include "macros.h"
#include "src/overlays/actors/ovl_Boss_Dodongo/z_boss_dodongo.h"
#include "src/overlays/actors/ovl_Boss_Ganondrof/z_boss_ganondrof.h"
#include "src/overlays/actors/ovl_Boss_Mo/z_boss_mo.h"
#include "src/overlays/actors/ovl_Boss_Sst/z_boss_sst.h"
#include "src/overlays/actors/ovl_Boss_Va/z_boss_va.h"
extern PlayState* gPlayState;
}

/**
 * See CoopBoss.h for the design.
 *
 * BOSS_HP        { sceneNum, roomNum, actorId, hp }   sent on every local HP drop + once a second
 * BOSS_DEFEATED  { sceneNum, roomNum, actorId }       sent when the local player kills the boss
 *
 * Both are only sent to teammates that are in the same scene.
 */

namespace {

// How often (in game frames, 20/s) the current boss HP is re-broadcast. Covers late
// joiners, respawns after a game over and lost packets.
constexpr int kHeartbeatFrames = 20;

struct CoopBossState {
    const PlayState* play = nullptr;
    s16 sceneNum = -1;
    bool defeated = false; // boss of this scene visit was defeated (locally or remotely)
    s32 lastSentHp = -1;   // last HP we told the others about
    int heartbeatTimer = 0;
    // Fractional damage carried over between hits, per actor instance.
    std::unordered_map<const Actor*, float> carry;
};

CoopBossState sState;

void ResetStateIfSceneChanged() {
    if (gPlayState != sState.play || (gPlayState != nullptr && gPlayState->sceneNum != sState.sceneNum)) {
        sState = CoopBossState();
        sState.play = gPlayState;
        sState.sceneNum = gPlayState != nullptr ? gPlayState->sceneNum : -1;
    }
}

bool IsActive(Anchor* anchor) {
    return anchor != nullptr && anchor->isConnected && anchor->IsSaveLoaded() && gPlayState != nullptr &&
           anchor->roomState.sharedBosses;
}

std::string OwnTeamId() {
    return CVarGetString(CVAR_REMOTE_ANCHOR("TeamId"), "default");
}

// A teammate that is online, in game, and in the same scene and room as us.
bool IsTeammateInMyRoom(const AnchorClient& client, const std::string& teamId) {
    return !client.self && client.online && client.isSaveLoaded && client.teamId == teamId &&
           client.sceneNum == gPlayState->sceneNum && client.curRoomNum == (s8)gPlayState->roomCtx.curRoom.num;
}

int PlayersInMyRoom(Anchor* anchor) {
    const std::string teamId = OwnTeamId();
    int count = 1;
    for (const auto& [clientId, client] : anchor->clients) {
        if (IsTeammateInMyRoom(client, teamId)) {
            count++;
        }
    }
    return count;
}

// Is this the actor that owns the health pool of a shared boss?
bool IsSharedBossActor(const Actor* actor) {
    switch (actor->id) {
        case ACTOR_BOSS_GOMA:    // Gohma
        case ACTOR_BOSS_DODONGO: // King Dodongo
        case ACTOR_BOSS_FD:      // Volvagia (the hole form writes into this actor's health)
        case ACTOR_BOSS_GANON2:  // Ganon
            return true;
        case ACTOR_BOSS_VA: // Barinade (body)
            return actor->params == BOSSVA_BODY;
        case ACTOR_BOSS_GANONDROF: // Phantom Ganon (not the painting decoy)
            return actor->params == GND_REAL_BOSS;
        case ACTOR_BOSS_MO: // Morpha (core; its params become 0 after init, tentacles are >= 100)
            return actor->params < BOSSMO_TENTACLE;
        case ACTOR_BOSS_SST: // Bongo Bongo (head)
            return actor->params == BONGO_HEAD;
        case ACTOR_BOSS_TW: // Twinrova (merged form, TW_TWINROVA in z_boss_tw.c)
            return actor->params == 2;
        case ACTOR_BOSS_GANON: // Ganondorf (effects use params >= 0x64)
            return actor->params < 0x64;
        default:
            return false;
    }
}

// Barinade's final-phase pool is a few HP that refill between sub-phases, so its health
// is not shared; only its defeat is.
bool HasSharedHealth(const Actor* actor) {
    return actor->id != ACTOR_BOSS_VA;
}

Actor* FindSharedBoss(s16 actorId = -1) {
    if (gPlayState == nullptr) {
        return nullptr;
    }
    for (Actor* actor = gPlayState->actorCtx.actorLists[ACTORCAT_BOSS].head; actor != nullptr; actor = actor->next) {
        if (actor->update == nullptr || !IsSharedBossActor(actor)) {
            continue;
        }
        if (actorId == -1 || actor->id == actorId) {
            return actor;
        }
    }
    return nullptr;
}

s32 GetBossHp(Actor* actor) {
    if (actor->id == ACTOR_BOSS_DODONGO) {
        return ((BossDodongo*)actor)->health;
    }
    return (s8)actor->colChkInfo.health;
}

void SetBossHp(Actor* actor, s32 hp) {
    if (actor->id == ACTOR_BOSS_DODONGO) {
        ((BossDodongo*)actor)->health = (s16)hp;
    } else {
        actor->colChkInfo.health = (u8)hp;
    }
}

bool IsTransitioning() {
    return gPlayState->transitionTrigger != TRANS_TRIGGER_OFF || gPlayState->transitionMode != TRANS_MODE_OFF ||
           gPlayState->gameOverCtx.state != GAMEOVER_INACTIVE;
}

} // namespace

// MARK: - Damage scaling (called from the game's boss damage code)

extern "C" int CoopBoss_ScaleDamage(struct Actor* actor, int damage) {
    if (actor == nullptr || damage <= 0 || actor->category != ACTORCAT_BOSS) {
        return damage;
    }

    Anchor* anchor = Anchor::Instance;
    if (!IsActive(anchor) || anchor->roomState.bossHealthBonus == 0) {
        return damage;
    }

    const int players = PlayersInMyRoom(anchor);
    if (players <= 1) {
        return damage;
    }

    // effective HP = native HP * (1 + bonus% * extra players)
    const float hpMultiplier = 1.0f + (anchor->roomState.bossHealthBonus / 100.0f) * (float)(players - 1);

    ResetStateIfSceneChanged();
    float& carry = sState.carry[actor];
    const float exact = (float)damage / hpMultiplier + carry;
    // Small epsilon so 0.5 + 0.5 reliably lands on 1 instead of 0.99999.
    const int applied = (int)std::floor(exact + 0.0001f);
    carry = std::max(0.0f, exact - (float)applied);

    return applied;
}

// MARK: - Packets

void Anchor::SendPacket_BossHp(s16 actorId, s32 hp) {
    if (!IsSaveLoaded()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = BOSS_HP;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["roomNum"] = (s8)gPlayState->roomCtx.curRoom.num;
    payload["actorId"] = actorId;
    payload["hp"] = hp;
    payload["quiet"] = true;

    const std::string teamId = OwnTeamId();
    for (auto& [clientId, client] : clients) {
        if (IsTeammateInMyRoom(client, teamId)) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

void Anchor::HandlePacket_BossHp(nlohmann::json payload) {
    if (!IsActive(this) || IsTransitioning()) {
        return;
    }

    uint32_t clientId = payload.at("clientId").get<uint32_t>();
    if (!clients.contains(clientId) || clients[clientId].teamId != OwnTeamId()) {
        return;
    }
    if (payload.at("sceneNum").get<s16>() != gPlayState->sceneNum ||
        payload.at("roomNum").get<s8>() != (s8)gPlayState->roomCtx.curRoom.num) {
        return;
    }

    ResetStateIfSceneChanged();
    if (sState.defeated) {
        return;
    }

    Actor* boss = FindSharedBoss(payload.at("actorId").get<s16>());
    if (boss == nullptr || !HasSharedHealth(boss)) {
        return;
    }

    const s32 remoteHp = payload.at("hp").get<s32>();
    const s32 localHp = GetBossHp(boss);
    // Never go below 1: the killing blow always goes through the game's own damage code
    // (locally) or through BOSS_DEFEATED (remotely).
    const s32 targetHp = std::max(remoteHp, (s32)1);
    if (localHp > 0 && targetHp < localHp) {
        SetBossHp(boss, targetHp);
        sState.lastSentHp = targetHp; // the others already know this value, don't echo it
        Actor_SetColorFilter(boss, 0x4000, 255, 0, 8);
    }
}

void Anchor::SendPacket_BossDefeated(s16 actorId) {
    if (!IsSaveLoaded()) {
        return;
    }

    nlohmann::json payload;
    payload["type"] = BOSS_DEFEATED;
    payload["sceneNum"] = gPlayState->sceneNum;
    payload["roomNum"] = (s8)gPlayState->roomCtx.curRoom.num;
    payload["actorId"] = actorId;

    const std::string teamId = OwnTeamId();
    for (auto& [clientId, client] : clients) {
        if (IsTeammateInMyRoom(client, teamId)) {
            payload["targetClientId"] = clientId;
            SendJsonToRemote(payload);
        }
    }
}

void Anchor::HandlePacket_BossDefeated(nlohmann::json payload) {
    if (!IsActive(this)) {
        return;
    }

    uint32_t clientId = payload.at("clientId").get<uint32_t>();
    if (!clients.contains(clientId) || clients[clientId].teamId != OwnTeamId()) {
        return;
    }
    const s8 roomNum = payload.at("roomNum").get<s8>();
    if (payload.at("sceneNum").get<s16>() != gPlayState->sceneNum || roomNum != (s8)gPlayState->roomCtx.curRoom.num) {
        return;
    }

    ResetStateIfSceneChanged();
    if (sState.defeated) {
        return; // we already killed it ourselves, or already handled this
    }

    const s16 actorId = payload.at("actorId").get<s16>();
    Actor* boss = FindSharedBoss(actorId);
    if (boss == nullptr) {
        return; // no live boss on our side (room already cleared)
    }
    sState.defeated = true;
    if (HasSharedHealth(boss) && GetBossHp(boss) <= 0) {
        return; // our own boss is already dying (both landed the killing blow): let it play out
    }

    Notification::Emit({
        .prefix = clients[clientId].name,
        .message = "defeated the boss",
    });

    if (actorId == ACTOR_BOSS_GANON) {
        // Ganondorf: follow the team into the tower escape, like the end of his death cutscene.
        if (!IsTransitioning()) {
            gPlayState->nextEntranceIndex = ENTR_GANONS_TOWER_COLLAPSE_EXTERIOR_0;
            gPlayState->transitionTrigger = TRANS_TRIGGER_START;
            gPlayState->transitionType = TRANS_TYPE_FADE_WHITE_FAST;
        }
        return;
    }

    if (actorId == ACTOR_BOSS_GANON2) {
        // Ganon: go to the ending, like the end of the final blow cutscene.
        if (!IsTransitioning()) {
            gPlayState->nextEntranceIndex = ENTR_CHAMBER_OF_THE_SAGES_0;
            gSaveContext.nextCutsceneIndex = 0xFFF2;
            gPlayState->transitionTrigger = TRANS_TRIGGER_START;
            gPlayState->transitionType = TRANS_TYPE_FADE_WHITE;
            gPlayState->linkAgeOnLoad = 1;
        }
        return;
    }

    // Dungeon bosses: mark the room as cleared and reload it. On load every boss takes its
    // own "already defeated" path, which spawns the blue warp and the heart container.
    Flags_SetClear(gPlayState, roomNum);
    gSaveContext.sceneFlags[gPlayState->sceneNum].clear |= (1 << roomNum);

    if (!IsTransitioning()) {
        gPlayState->nextEntranceIndex = gSaveContext.entranceIndex;
        gPlayState->transitionTrigger = TRANS_TRIGGER_START;
        gPlayState->transitionType = TRANS_TYPE_FADE_BLACK_FAST;
        gSaveContext.nextTransitionType = TRANS_TYPE_FADE_BLACK_FAST;
    }
}

// MARK: - Hooks

void Anchor::RegisterCoopBossHooks() {
    // A reload of the same scene keeps the same PlayState address, so reset explicitly.
    COND_HOOK(OnSceneInit, isConnected, [&](int16_t sceneNum) {
        sState = CoopBossState();
        sState.play = gPlayState;
        sState.sceneNum = sceneNum;
    });

    // The local player landed the killing blow.
    COND_HOOK(OnBossDefeat, isConnected, [&](void* refActor) {
        Actor* actor = (Actor*)refActor;
        if (!IsActive(this) || actor == nullptr) {
            return;
        }

        ResetStateIfSceneChanged();
        if (sState.defeated) {
            return;
        }
        sState.defeated = true;

        // Volvagia's hook fires on the hole form; the shared actor is the flying form.
        s16 actorId = actor->id == ACTOR_BOSS_FD2 ? (s16)ACTOR_BOSS_FD : actor->id;
        SendPacket_BossDefeated(actorId);
    });

    // Broadcast our boss HP when it drops, and once a second as a heartbeat.
    COND_HOOK(OnPlayerUpdate, isConnected, [&]() {
        if (!IsActive(this)) {
            return;
        }

        ResetStateIfSceneChanged();
        if (sState.defeated || IsTransitioning()) {
            return;
        }

        Actor* boss = FindSharedBoss();
        if (boss == nullptr || !HasSharedHealth(boss)) {
            return;
        }

        const s32 hp = GetBossHp(boss);
        if (hp <= 0) {
            return;
        }

        const bool dropped = sState.lastSentHp != -1 && hp < sState.lastSentHp;
        if (dropped || ++sState.heartbeatTimer >= kHeartbeatFrames) {
            sState.heartbeatTimer = 0;
            sState.lastSentHp = hp;
            SendPacket_BossHp(boss->id, hp);
        }
    });
}
