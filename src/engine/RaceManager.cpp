#include "RaceManager.h"

#include "AllActors.h"
#include "World.h"
#include "port/Game.h"
#include "engine/editor/Editor.h"
#include "engine/editor/SceneManager.h"
#include "engine/RandomItemTable.h"

extern "C" {
#include "render_courses.h"
}

RaceManager::RaceManager(World& world) : WorldContext(world) {
}

std::unordered_map<uintptr_t, std::shared_ptr<Vtx>> mirroredVtxCache;

// Populates a collision mesh for mirror mode
extern "C" void add_triangle_to_collision_mesh(Vtx* vtx1, Vtx* vtx2, Vtx* vtx3, Vtx** outVtx1, Vtx** outVtx2, Vtx** outVtx3) {
    if (gIsMirrorMode != 0) {
        auto getOrCreateMirrored = [](Vtx* original) -> Vtx* {
            uintptr_t key = reinterpret_cast<uintptr_t>(original);
            
            auto it = mirroredVtxCache.find(key);
            if (it != mirroredVtxCache.end()) {
                return it->second.get();
            }

            auto newVtx = std::make_shared<Vtx>(*original);
            newVtx->v.ob[0] = -newVtx->v.ob[0];

            mirroredVtxCache[key] = newVtx;

            return newVtx.get();
        };

        Vtx* m1 = getOrCreateMirrored(vtx1);
        Vtx* m2 = getOrCreateMirrored(vtx2);
        Vtx* m3 = getOrCreateMirrored(vtx3);

        // don't invert winding here, already done in the gfx
        *outVtx1 = m1;
        *outVtx2 = m2;
        *outVtx3 = m3;

    } else {
        // Pas de miroir, on passe les originaux
        *outVtx1 = vtx1;
        *outVtx2 = vtx2;
        *outVtx3 = vtx3;
    }
}

void RaceManager::Load() {
    auto* track = GetWorld()->GetTrack();
    if (track) {
        mirroredVtxCache.clear();
        track->Load();
    } else {
        printf("[RaceManager] [Load] Track was nullptr\n");
    }
}

void RaceManager::PreInit() {
    // Ruleset options
    if (CVarGetInteger("gDisableItemboxes", false) == true) {
        gPlaceItemBoxes = false;
    } else {
        gPlaceItemBoxes = true;
    }

    RaceManager::SetItemTables();
}

void RaceManager::BeginPlay() {
    auto track = WorldContext.GetTrack();

    if (track) {
        // Do not spawn finishline in credits or battle mode. And if bSpawnFinishline.
        if ((gGamestate != CREDITS_SEQUENCE) && (gModeSelection != BATTLE)) {
            if (track->bSpawnFinishline) {
                if (track->FinishlineSpawnPoint.has_value()) {
                    AFinishline* finishline = AFinishline::Spawn(track->FinishlineSpawnPoint.value(), IRotator(0, 0, 0));
                    finishline->bIsFinishline = true;
                } else {
                    AFinishline* finishline = AFinishline::Spawn();
                    finishline->bIsFinishline = true;
                }
            }
        }
        gEditor.AddLight("Sun", nullptr, D_800DC610[1].l->l.dir);

        track->BeginPlay();
    }
}

void RaceManager::PostInit() {
    // Ruleset options
    if (CVarGetInteger("gAllThwompsAreMarty", false) == true) {
        for (auto& object : GetWorld()->Objects) {
            if (OThwomp* thwomp = dynamic_cast<OThwomp*>(object.get())) {
                gObjectList[thwomp->_objectIndex].unk_0D5 = OThwomp::States::JAILED; // Sets all the thwomp behaviour flags to marty
                thwomp->Behaviour = OThwomp::States::JAILED;
            }
        }
    }

    if (CVarGetInteger("gAllBombKartsChase", false) == true) {
        for (auto& object : GetWorld()->Objects) {
            if (OBombKart* kart = dynamic_cast<OBombKart*>(object.get())) {
                kart->Behaviour = OBombKart::States::CHASE;
            }
        }
    }

    if (CVarGetInteger("gGoFish", false) == true) {
        OTrophy::Spawn(FVector(0,0,0), OTrophy::TrophyType::GOLD, OTrophy::Behaviour::GO_FISH);
    }
}

void RaceManager::SetItemTables() {
    std::optional<std::string> humanTableName;
    std::optional<std::string> cpuTableName;

    switch(gModeSelection) {
        case GRAND_PRIX:
            if (CVarGetInteger("gHarderCPU", false) == true) {
                humanTableName = "mk:hard_cpu_grand_prix";
                cpuTableName = "mk:hard_cpu_grand_prix";
            } else { // normal gameplay
                humanTableName = "mk:human_grand_prix";
                cpuTableName = "mk:cpu_grand_prix";
            }
            break;
        case VERSUS:
            switch (gPlayerCountSelection1) {
                case TWO_PLAYERS_SELECTED:
                    humanTableName = "mk:versus_2p";
                    break;
                case THREE_PLAYERS_SELECTED:
                    humanTableName = "mk:versus_3p";
                    break;
                case FOUR_PLAYERS_SELECTED:
                    humanTableName = "mk:versus_4p";
                    break;
            }
            break;
        case BATTLE:
            humanTableName = "mk:battle";
            break;
    }
    if (humanTableName.has_value()) {
        mHumanItemTable = gItemTableRegistry.Get(humanTableName.value());
    } else {
        mHumanItemTable = nullptr;
    }
    if (cpuTableName.has_value()) {
        mCPUItemTable = gItemTableRegistry.Get(cpuTableName.value());
    } else {
        mCPUItemTable = nullptr;
    }
    printf("[RaceManager] Selected human item probability table %s\n", humanTableName.value_or("none").c_str());
    printf("[RaceManager] Selected cpu item probability table %s\n", cpuTableName.value_or("none").c_str());
}

extern "C" uint16_t random_int(uint16_t); // racing/math_util.h - returns 0..arg inclusive

// Item modes from the race setup screen (src/race_mods_menu.c), all gated by gItemModeCPU for CPU rolls.
// gItemMode: 0 = stock weighted tables, 1 = truly random (uniform over every stock item, ignores rank),
// 2 = everyone draws the same chosen item, 3 = frantic (power items only), 4 = triples (stock roll, then
// every item upgrades to its triple variant), 5 = inverted (stock tables with the rank flipped - the
// leader draws last-place luck and vice versa).
static bool RaceModsApplies(bool isCpu) {
    if (CVarGetInteger("gItemMode", 0) == 0) {
        return false;
    }
    return !isCpu || CVarGetInteger("gItemModeCPU", 1) != 0;
}

// Full-replacement modes (1-3). Returns -1 when the table roll should run instead.
static int16_t RaceModsItemOverride(bool isCpu) {
    if (!RaceModsApplies(isCpu)) {
        return -1;
    }
    switch (CVarGetInteger("gItemMode", 0)) {
        case 1: {
            // A pure uniform across the complete item table: every item, equal odds, every pull,
            // rank ignored - with one exception. A flat 1-in-15 lightning rate across eight karts
            // means someone bolts the field every few seconds, so a bolt result keeps only 1 roll
            // in 8 and the reroll picks from the other 14 items. Net bolt rate: about 1 box in 120.
            int16_t item = (int16_t) (1 + random_int(ITEM_MAX - 2));
            if (item == ITEM_THUNDERBOLT && random_int(7) != 0) {
                item = (int16_t) (1 + random_int(ITEM_MAX - 3));
                if (item >= ITEM_THUNDERBOLT) {
                    item++;
                }
            }
            return item;
        }
        case 2: {
            int32_t item = CVarGetInteger("gItemModeItem", ITEM_BANANA);
            if (item > ITEM_NONE && item < ITEM_MAX) {
                return (int16_t) item;
            }
            return -1;
        }
        case 3: {
            // Pure chaos: only the heavy hitters, with the blue shell and lightning showing up twice
            // as often as the rest.
            static const uint8_t kFrantic[] = { ITEM_TRIPLE_RED_SHELL, ITEM_BLUE_SPINY_SHELL,
                                                ITEM_BLUE_SPINY_SHELL, ITEM_THUNDERBOLT,
                                                ITEM_THUNDERBOLT,      ITEM_STAR,
                                                ITEM_SUPER_MUSHROOM,   ITEM_TRIPLE_MUSHROOM };
            return (int16_t) kFrantic[random_int(7)];
        }
    }
    return -1;
}

// Mode 5 (inverted) flips the rank fed to the stock tables.
static uint32_t RaceModsAdjustRank(uint32_t rank, bool isCpu) {
    if (RaceModsApplies(isCpu) && CVarGetInteger("gItemMode", 0) == 5 && rank < NUM_PLAYERS) {
        return (NUM_PLAYERS - 1) - rank;
    }
    return rank;
}

// Mode 4 (triples) upgrades a stock roll to the triple variant where one exists.
static int16_t RaceModsPostRoll(int16_t item, bool isCpu) {
    if (RaceModsApplies(isCpu) && CVarGetInteger("gItemMode", 0) == 4) {
        int16_t upgraded = item;
        switch (item) {
            case ITEM_BANANA:
                upgraded = ITEM_BANANA_BUNCH;
                break;
            case ITEM_GREEN_SHELL:
                upgraded = ITEM_TRIPLE_GREEN_SHELL;
                break;
            case ITEM_RED_SHELL:
                upgraded = ITEM_TRIPLE_RED_SHELL;
                break;
            case ITEM_MUSHROOM:
            case ITEM_DOUBLE_MUSHROOM:
                upgraded = ITEM_TRIPLE_MUSHROOM;
                break;
        }
        printf("[RaceMods] triples roll: %d -> %d (cpu=%d)\n", item, upgraded, isCpu ? 1 : 0); // TEMP diag
        return upgraded;
    }
    return item;
}

// Infected mode (gRaceMode 5): the blue shell and lightning hit the leader or the whole field
// regardless of position - unfair in a mode decided by contact chases (the community pitch that
// shaped this mode called both out). They remap to strong but aimed items. Runs on EVERY roll
// path, item modes included.
static int16_t InfectedModeFilter(int16_t item) {
    if (CVarGetInteger("gRaceMode", 0) == 5) {
        if (item == ITEM_BLUE_SPINY_SHELL) {
            return ITEM_TRIPLE_RED_SHELL;
        }
        if (item == ITEM_THUNDERBOLT) {
            return ITEM_SUPER_MUSHROOM;
        }
    }
    return item;
}

extern "C" int16_t RaceManager_GetRandomHumanItem(uint32_t rank) {
    int16_t forced = RaceModsItemOverride(false);
    if (forced >= 0) {
        return InfectedModeFilter(forced);
    }

    auto& raceManager = GetWorld()->GetRaceManager();

    auto* table = raceManager.GetHumanItemTable();
    if (nullptr == table) {
        printf("[RaceManager_GetRandomHumanItem] Item table nullptr, giving player a none item\n");
        return ITEM_NONE;
    }

    return InfectedModeFilter(RaceModsPostRoll(table->Roll(RaceModsAdjustRank(rank, false)), false));
}

extern "C" int16_t RaceManager_GetRandomCPUItem(uint32_t rank) {
    int16_t forced = RaceModsItemOverride(true);
    if (forced >= 0) {
        return InfectedModeFilter(forced);
    }

    auto& raceManager = GetWorld()->GetRaceManager();
    auto* table = raceManager.GetCPUItemTable();
    if (nullptr == table) {
        printf("[RaceManager_GetRandomCPUItem] Item table nullptr, giving player a none item\n");
        return ITEM_NONE;
    }

    return InfectedModeFilter(RaceModsPostRoll(table->Roll(RaceModsAdjustRank(rank, true)), true));
}
