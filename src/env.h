#ifndef IMPULSE_WARS_ENV_H
#define IMPULSE_WARS_ENV_H

#include "game.h"
#include "map.h"
#include "scripted_bot.h"
#include "settings.h"
#include "types.h"

// autopdx can't parse raylib's headers for some reason, but that's ok
// because the Cython code doesn't need to directly use raylib anyway
// include the full render header if we're compiling C code, otherwise
// just declare the necessary functions the Cython code needs
#ifndef AUTOPXD
#include "render.h"
#else
rayClient *createRayClient();
void destroyRayClient(rayClient *client);
typedef struct Vector2 {
    float x;
    float y;
} Vector2;
#endif

const uint8_t TWO_BIT_MASK = 0x3;
const uint8_t THREE_BIT_MASK = 0x7;
const uint8_t FOUR_BIT_MASK = 0xf;

logBuffer *createLogBuffer(uint16_t capacity) {
    logBuffer *logs = (logBuffer *)fastCalloc(1, sizeof(logBuffer));
    logs->logs = (logEntry *)fastCalloc(capacity, sizeof(logEntry));
    logs->size = 0;
    logs->capacity = capacity;
    return logs;
}

void destroyLogBuffer(logBuffer *buffer) {
    fastFree(buffer->logs);
    fastFree(buffer);
}

void addLogEntry(logBuffer *logs, logEntry *log) {
    if (logs->size == logs->capacity) {
        return;
    }
    logs->logs[logs->size] = *log;
    logs->size += 1;
}

logEntry aggregateAndClearLogBuffer(uint8_t numDrones, logBuffer *logs) {
    logEntry log = {0};
    if (logs->size == 0) {
        return log;
    }

    DEBUG_LOGF("aggregating logs, size: %d", logs->size);

    const float logSize = logs->size;
    for (uint16_t i = 0; i < logs->size; i++) {
        log.length += logs->logs[i].length / logSize;

        for (uint8_t j = 0; j < numDrones; j++) {
            log.stats[j].reward += logs->logs[i].stats[j].reward / logSize;
            log.stats[j].wins += logs->logs[i].stats[j].wins / logSize;

            for (uint8_t k = 0; k < NUM_WEAPONS; k++) {
                log.stats[j].distanceTraveled += logs->logs[i].stats[j].distanceTraveled / logSize;
                log.stats[j].absDistanceTraveled += logs->logs[i].stats[j].absDistanceTraveled / logSize;
                log.stats[j].shotsFired[k] += logs->logs[i].stats[j].shotsFired[k] / logSize;
                log.stats[j].shotsHit[k] += logs->logs[i].stats[j].shotsHit[k] / logSize;
                log.stats[j].shotsTaken[k] += logs->logs[i].stats[j].shotsTaken[k] / logSize;
                log.stats[j].ownShotsTaken[k] += logs->logs[i].stats[j].ownShotsTaken[k] / logSize;
                log.stats[j].weaponsPickedUp[k] += logs->logs[i].stats[j].weaponsPickedUp[k] / logSize;
                log.stats[j].shotDistances[k] += logs->logs[i].stats[j].shotDistances[k] / logSize;
            }
        }
    }

    logs->size = 0;
    return log;
}

// returns a cell index that is closest to pos that isn't cellIdx
uint16_t findNearestCell(const env *e, const b2Vec2 pos, const uint16_t cellIdx) {
    uint8_t cellOffsets[8][2] = {
        {-1, 0},  // left
        {1, 0},   // right
        {0, -1},  // up
        {0, 1},   // down
        {-1, -1}, // top-left
        {1, -1},  // top-right
        {-1, 1},  // bottom-left
        {1, 1},   // bottom-right
    };

    uint16_t closestCell = cellIdx;
    float minDistance = FLT_MAX;
    const uint8_t cellRow = cellIdx / e->columns;
    const uint8_t cellCol = cellIdx % e->columns;
    for (uint8_t i = 0; i < 8; i++) {
        const uint16_t newCellIdx = ((cellRow + cellOffsets[i][0]) * e->columns) + (cellCol + cellOffsets[i][1]);
        const mapCell *cell = safe_array_get_at(e->cells, cellIdx);
        if (minDistance != fminf(minDistance, b2Distance(pos, cell->pos))) {
            closestCell = newCellIdx;
        }
    }

    return closestCell;
}

static inline float scaleAmmo(const env *e, const droneEntity *drone) {
    int8_t maxAmmo = weaponAmmo(e->defaultWeapon->type, drone->weaponInfo->type);
    float scaledAmmo = 0;
    if (drone->ammo != INFINITE) {
        scaledAmmo = scaleValue(drone->ammo, maxAmmo, true);
    }
    return scaledAmmo;
}

void computeMapObs(env *e, const uint8_t agentIdx, const uint16_t startOffset) {
    const droneEntity *drone = safe_array_get_at(e->drones, agentIdx);
    const b2Vec2 dronePos = drone->pos.pos;
    const int16_t droneCellIdx = entityPosToCellIdx(e, dronePos);
    if (droneCellIdx == -1) {
        ERRORF("agent drone is out of bounds at %f %f", dronePos.x, dronePos.y);
    }
    const uint8_t droneCellRow = droneCellIdx % e->rows;
    const uint8_t droneCellCol = droneCellIdx / e->columns;

    const int8_t startRow = droneCellRow - (MAP_OBS_ROWS / 2);
    const int8_t startCol = droneCellCol - (MAP_OBS_COLUMNS / 2);

    const int8_t endRow = droneCellRow + (MAP_OBS_ROWS / 2);
    const int8_t endCol = droneCellCol + (MAP_OBS_COLUMNS / 2);

    // compute map layout, and discretized positions of weapon pickups
    bool pastEndOfMap = false;
    uint16_t offset = startOffset;
    for (int8_t col = startCol; col <= endCol; col++) {
        if (pastEndOfMap) {
            break;
        }
        for (int8_t row = startRow; row <= endRow; row++) {
            if (row < 0 || row >= e->rows || col < 0) {
                offset++;
                continue;
            } else if (col >= e->columns) {
                pastEndOfMap = true;
                break;
            }

            const int16_t cellIdx = cellIndex(e, row, col);
            const mapCell *cell = safe_array_get_at(e->cells, cellIdx);
            if (cell->ent == NULL) {
                offset++;
                continue;
            }

            if (entityTypeIsWall(cell->ent->type)) {
                e->obs[offset] = ((cell->ent->type + 1) & TWO_BIT_MASK) << 5;
            } else if (cell->ent->type == WEAPON_PICKUP_ENTITY) {
                e->obs[offset] |= 1 << 3;
            }

            offset++;
        }
    }
    ASSERTF(!pastEndOfMap || offset <= startOffset + MAP_OBS_SIZE, "offset %u startOffset %u", offset, startOffset);
    ASSERTF(pastEndOfMap || offset == startOffset + MAP_OBS_SIZE, "offset %u startOffset %u", offset, startOffset);

    // compute discretized location of floating walls on grid
    for (size_t i = 0; i < cc_array_size(e->floatingWalls); i++) {
        const wallEntity *wall = safe_array_get_at(e->floatingWalls, i);
        const b2Vec2 pos = b2Body_GetPosition(wall->bodyID);
        int16_t cellIdx = entityPosToCellIdx(e, pos);
        if (cellIdx == -1) {
            ERRORF("floating wall %zu out of bounds at position %f %f", i, pos.x, pos.y);
        }
        const uint8_t cellRow = cellIdx % e->rows;
        if (cellRow < startRow || cellRow > endRow) {
            continue;
        }
        const uint8_t cellCol = cellIdx / e->columns;
        if (cellCol < startCol || cellCol > endCol) {
            continue;
        }

        offset = startOffset + (cellRow - startRow + ((cellCol - startCol) * MAP_OBS_COLUMNS));
        ASSERTF(offset <= startOffset + MAP_OBS_SIZE, "offset: %d", offset);
        e->obs[offset] = ((wall->type + 1) & TWO_BIT_MASK) << 5;
        e->obs[offset] |= 1 << 4;
    }

    // compute discretized location and index of drones on grid
    uint8_t newDroneIdx = 1;
    uint16_t droneCells[e->numDrones];
    memset(droneCells, 0x0, sizeof(droneCells));
    for (uint8_t i = 0; i < cc_array_size(e->drones); i++) {
        b2Vec2 pos = dronePos;
        int16_t cellIdx = droneCellIdx;
        if (i != agentIdx) {
            droneEntity *otherDrone = safe_array_get_at(e->drones, i);
            pos = otherDrone->pos.pos;
            cellIdx = entityPosToCellIdx(e, pos);
            if (cellIdx == -1) {
                ERRORF("drone %d out of bounds at position %f %f", i, pos.x, pos.y);
            }
        }

        // ensure drones do not share cells in the observation
        if (i != 0) {
            for (uint8_t j = 0; j < i; j++) {
                if (droneCells[j] == cellIdx) {
                    cellIdx = findNearestCell(e, pos, cellIdx);
                    break;
                }
            }
        }
        const uint8_t cellRow = cellIdx % e->rows;
        if (cellRow < startRow || cellRow > endRow) {
            continue;
        }
        const uint8_t cellCol = cellIdx / e->columns;
        if (cellCol < startCol || cellCol > endCol) {
            continue;
        }
        droneCells[i] = cellIdx;

        // set the agent's drone to be drone 0
        uint8_t droneIdx = 0;
        if (i != agentIdx) {
            droneIdx = newDroneIdx++;
        }

        offset = startOffset + (cellRow - startRow + ((cellCol - startCol) * MAP_OBS_COLUMNS));
        ASSERTF(offset <= startOffset + MAP_OBS_SIZE, "offset: %d", offset);
        e->obs[offset] |= ((droneIdx + 1) & THREE_BIT_MASK);
    }
}

void computeObs(env *e) {
    memset(e->obs, 0x0, e->obsBytes * e->numAgents);

    for (uint8_t agentIdx = 0; agentIdx < e->numAgents; agentIdx++) {
        // compute discrete map observations
        uint16_t mapObsOffset = e->obsBytes * agentIdx;
        const uint16_t mapObsStart = mapObsOffset;
        computeMapObs(e, agentIdx, mapObsOffset);

        // compute continuous scalar observations
        uint16_t scalarObsOffset = 0;
        const uint16_t scalarObsStart = mapObsStart + e->mapObsBytes;
        float *scalarObs = (float *)(e->obs + scalarObsStart);

        const droneEntity *agentDrone = safe_array_get_at(e->drones, agentIdx);
        const b2Vec2 agentDronePos = agentDrone->pos.pos;

        // compute type and position of N nearest walls
        kdres *nearWalls = kd_nearest_n(e->wallTree, agentDronePos.x, agentDronePos.y, NUM_NEAR_WALL_OBS);
        for (uint8_t i = 0; i < NUM_NEAR_WALL_OBS; i++) {
            const wallEntity *wall = kd_res_item_data(nearWalls);
            kd_res_next(nearWalls);

            scalarObsOffset = NEAR_WALL_TYPES_OBS_OFFSET + i;
            ASSERTF(scalarObsOffset <= NEAR_WALL_POS_OBS_OFFSET, "offset: %d", scalarObsOffset);
            scalarObs[scalarObsOffset] = wall->type;

            scalarObsOffset = NEAR_WALL_POS_OBS_OFFSET + (i * NEAR_WALL_POS_OBS_SIZE);
            ASSERTF(scalarObsOffset <= FLOATING_WALL_TYPES_OBS_OFFSET, "offset: %d", scalarObsOffset);
            ASSERT(wall->pos.valid);
            const b2Vec2 wallRelPos = b2Sub(wall->pos.pos, agentDronePos);
            scalarObs[scalarObsOffset++] = scaleValue(wallRelPos.x, MAX_X_POS, false);
            scalarObs[scalarObsOffset] = scaleValue(wallRelPos.y, MAX_Y_POS, false);
        }
        kd_res_free(nearWalls);

        // compute type, position, angle and velocity of floating walls
        for (size_t i = 0; i < cc_array_size(e->floatingWalls); i++) {
            const wallEntity *wall = safe_array_get_at(e->floatingWalls, i);
            const b2Transform wallTransform = b2Body_GetTransform(wall->bodyID);
            const b2Vec2 wallRelPos = b2Sub(wallTransform.p, agentDronePos);
            const float angle = b2Rot_GetAngle(wallTransform.q);
            const b2Vec2 wallVel = b2Body_GetLinearVelocity(wall->bodyID);

            scalarObsOffset = FLOATING_WALL_TYPES_OBS_OFFSET + i;
            ASSERTF(scalarObsOffset <= FLOATING_WALL_INFO_OBS_OFFSET, "offset: %d", scalarObsOffset);
            scalarObs[scalarObsOffset] = wall->type + 1;

            scalarObsOffset = FLOATING_WALL_INFO_OBS_OFFSET + (i * FLOATING_WALL_INFO_OBS_SIZE);
            ASSERTF(scalarObsOffset <= WEAPON_PICKUP_TYPES_OBS_OFFSET, "offset: %d", scalarObsOffset);
            scalarObs[scalarObsOffset++] = scaleValue(wallRelPos.x, MAX_X_POS, false);
            scalarObs[scalarObsOffset++] = scaleValue(wallRelPos.y, MAX_Y_POS, false);
            scalarObs[scalarObsOffset++] = scaleValue(angle, MAX_ANGLE, false);
            scalarObs[scalarObsOffset++] = scaleValue(wallVel.x, MAX_SPEED, false);
            scalarObs[scalarObsOffset] = scaleValue(wallVel.y, MAX_SPEED, false);
        }

        // compute type and location of N nearest weapon pickups
        // TODO: use KD tree here
        for (uint8_t i = 0; i < cc_array_size(e->pickups); i++) {
            const weaponPickupEntity *pickup = safe_array_get_at(e->pickups, i);

            scalarObsOffset = WEAPON_PICKUP_TYPES_OBS_OFFSET + i;
            ASSERTF(scalarObsOffset <= WEAPON_PICKUP_POS_OBS_OFFSET, "offset: %d", scalarObsOffset);
            scalarObs[scalarObsOffset] = pickup->weapon + 1;

            scalarObsOffset = WEAPON_PICKUP_POS_OBS_OFFSET + (i * WEAPON_PICKUP_POS_OBS_SIZE);
            ASSERTF(scalarObsOffset <= PROJECTILE_TYPES_OBS_OFFSET, "offset: %d", scalarObsOffset);
            const b2Vec2 pickupRelPos = b2Sub(pickup->pos, agentDronePos);
            scalarObs[scalarObsOffset++] = scaleValue(pickupRelPos.x, MAX_X_POS, false);
            scalarObs[scalarObsOffset] = scaleValue(pickupRelPos.y, MAX_Y_POS, false);
        }

        // compute type and location of N projectiles
        uint8_t projIdx = 0;
        for (SNode *cur = e->projectiles->head; cur != NULL; cur = cur->next) {
            // TODO: handle better
            if (projIdx == NUM_PROJECTILE_OBS) {
                break;
            }
            const projectileEntity *projectile = (projectileEntity *)cur->data;

            scalarObsOffset = PROJECTILE_TYPES_OBS_OFFSET + projIdx;
            ASSERTF(scalarObsOffset <= PROJECTILE_POS_OBS_OFFSET, "offset: %d", scalarObsOffset);
            scalarObs[scalarObsOffset] = projectile->weaponInfo->type + 1;

            scalarObsOffset = PROJECTILE_POS_OBS_OFFSET + (projIdx * PROJECTILE_INFO_OBS_SIZE);
            ASSERTF(scalarObsOffset <= ENEMY_DRONE_OBS_OFFSET, "offset: %d", scalarObsOffset);
            const b2Vec2 projectileRelPos = b2Sub(projectile->lastPos, agentDronePos);
            scalarObs[scalarObsOffset++] = projectile->droneIdx + 1;
            scalarObs[scalarObsOffset++] = scaleValue(projectileRelPos.x, MAX_X_POS, false);
            scalarObs[scalarObsOffset] = scaleValue(projectileRelPos.y, MAX_Y_POS, false);

            projIdx++;
        }

        // compute enemy drone observations
        bool hitShot = false;
        bool tookShot = false;
        uint8_t processedDrones = 0;
        for (uint8_t i = 0; i < e->numDrones; i++) {
            if (i == agentIdx) {
                continue;
            }

            if (agentDrone->stepInfo.shotHit[i]) {
                hitShot = true;
            }
            if (agentDrone->stepInfo.shotTaken[i]) {
                tookShot = true;
            }

            const droneEntity *enemyDrone = safe_array_get_at(e->drones, i);
            const b2Vec2 enemyDroneRelPos = b2Sub(enemyDrone->pos.pos, agentDronePos);
            const float enemyDroneDistance = b2Distance(enemyDrone->pos.pos, agentDronePos);
            const b2Vec2 enemyDroneVel = b2Body_GetLinearVelocity(enemyDrone->bodyID);
            const b2Vec2 enemyDroneAccel = b2Sub(enemyDroneVel, enemyDrone->lastVelocity);
            const b2Vec2 enemyDroneRelNormPos = b2Normalize(b2Sub(enemyDrone->pos.pos, agentDronePos));
            const float enemyDroneAngle = atan2f(enemyDroneRelNormPos.y, enemyDroneRelNormPos.x);
            const float enemyDroneAimAngle = atan2f(enemyDrone->lastAim.y, enemyDrone->lastAim.x);

            const uint16_t enemyDroneObsOffset = ENEMY_DRONE_OBS_OFFSET + processedDrones;
            scalarObs[enemyDroneObsOffset] = enemyDrone->weaponInfo->type + 1;

            scalarObsOffset = ENEMY_DRONE_OBS_OFFSET + (e->numDrones - 1) + (processedDrones * (ENEMY_DRONE_OBS_SIZE - 1));
            scalarObs[scalarObsOffset++] = (float)agentDrone->inLineOfSight[i];
            scalarObs[scalarObsOffset++] = scaleValue(enemyDroneRelPos.x, MAX_X_POS, false);
            scalarObs[scalarObsOffset++] = scaleValue(enemyDroneRelPos.y, MAX_Y_POS, false);
            scalarObs[scalarObsOffset++] = scaleValue(enemyDroneDistance, MAX_DISTANCE, true); // TODO: ablate this
            scalarObs[scalarObsOffset++] = scaleValue(enemyDroneVel.x, MAX_SPEED, false);
            scalarObs[scalarObsOffset++] = scaleValue(enemyDroneVel.y, MAX_SPEED, false);
            scalarObs[scalarObsOffset++] = scaleValue(enemyDroneAccel.x, MAX_SPEED, false);
            scalarObs[scalarObsOffset++] = scaleValue(enemyDroneAccel.y, MAX_SPEED, false);
            scalarObs[scalarObsOffset++] = scaleValue(enemyDroneRelNormPos.x, 1.0f, false);
            scalarObs[scalarObsOffset++] = scaleValue(enemyDroneRelNormPos.y, 1.0f, false);
            scalarObs[scalarObsOffset++] = scaleValue(enemyDroneAngle, PI, false); // TODO: ablate this
            scalarObs[scalarObsOffset++] = scaleValue(enemyDrone->lastAim.x, 1.0f, false);
            scalarObs[scalarObsOffset++] = scaleValue(enemyDrone->lastAim.y, 1.0f, false);
            scalarObs[scalarObsOffset++] = scaleValue(enemyDroneAimAngle, PI, false);
            scalarObs[scalarObsOffset++] = scaleAmmo(e, enemyDrone);
            scalarObs[scalarObsOffset++] = scaleValue(enemyDrone->weaponCooldown, enemyDrone->weaponInfo->coolDown, true);
            scalarObs[scalarObsOffset++] = scaleValue(enemyDrone->charge, weaponCharge(enemyDrone->weaponInfo->type), true);

            processedDrones++;
            ASSERTF(scalarObsOffset == ENEMY_DRONE_OBS_OFFSET + (e->numDrones - 1) + (processedDrones * (ENEMY_DRONE_OBS_SIZE - 1)), "offset: %d", scalarObsOffset);
        }

        // compute active drone observations
        ASSERTF(scalarObsOffset == ENEMY_DRONE_OBS_OFFSET + ((e->numDrones - 1) * ENEMY_DRONE_OBS_SIZE), "offset: %d", scalarObsOffset);
        const b2Vec2 agentDroneVel = b2Body_GetLinearVelocity(agentDrone->bodyID);
        const b2Vec2 agentDroneAccel = b2Sub(agentDroneVel, agentDrone->lastVelocity);
        const float agentDroneAimAngle = atan2f(agentDrone->lastAim.y, agentDrone->lastAim.x);

        scalarObs[scalarObsOffset++] = agentDrone->weaponInfo->type + 1;
        scalarObs[scalarObsOffset++] = scaleValue(agentDronePos.x, MAX_X_POS, false);
        scalarObs[scalarObsOffset++] = scaleValue(agentDronePos.y, MAX_Y_POS, false);
        scalarObs[scalarObsOffset++] = scaleValue(agentDroneVel.x, MAX_SPEED, false);
        scalarObs[scalarObsOffset++] = scaleValue(agentDroneVel.y, MAX_SPEED, false);
        scalarObs[scalarObsOffset++] = scaleValue(agentDroneAccel.x, MAX_SPEED, false);
        scalarObs[scalarObsOffset++] = scaleValue(agentDroneAccel.y, MAX_SPEED, false);
        scalarObs[scalarObsOffset++] = scaleValue(agentDrone->lastAim.x, 1.0f, false);
        scalarObs[scalarObsOffset++] = scaleValue(agentDrone->lastAim.y, 1.0f, false);
        scalarObs[scalarObsOffset++] = scaleValue(agentDroneAimAngle, PI, false); // TODO: ablate this
        scalarObs[scalarObsOffset++] = scaleAmmo(e, agentDrone);
        scalarObs[scalarObsOffset++] = scaleValue(agentDrone->weaponCooldown, agentDrone->weaponInfo->coolDown, true);
        scalarObs[scalarObsOffset++] = scaleValue(agentDrone->charge, weaponCharge(agentDrone->weaponInfo->type), true);
        scalarObs[scalarObsOffset++] = hitShot;
        scalarObs[scalarObsOffset++] = tookShot;
        scalarObs[scalarObsOffset++] = agentDrone->stepInfo.ownShotTaken;

        ASSERTF(scalarObsOffset == ENEMY_DRONE_OBS_OFFSET + ((e->numDrones - 1) * ENEMY_DRONE_OBS_SIZE) + DRONE_OBS_SIZE, "offset: %d", scalarObsOffset);
        scalarObs[scalarObsOffset] = scaleValue(e->stepsLeft, ROUND_STEPS, true);
    }
}

void setupEnv(env *e) {
    e->needsReset = false;

    b2WorldDef worldDef = b2DefaultWorldDef();
    worldDef.gravity = (b2Vec2){.x = 0.0f, .y = 0.0f};
    e->worldID = b2CreateWorld(&worldDef);

    e->stepsLeft = ROUND_STEPS;
    e->suddenDeathSteps = SUDDEN_DEATH_STEPS;
    e->suddenDeathWallCounter = 0;

    DEBUG_LOG("creating map");
    // don't evaluate on the boring empty map
    uint8_t firstMap = 0;
    if (!e->isTraining) {
        firstMap = 1;
    }
    const int mapIdx = randInt(&e->randState, firstMap, NUM_MAPS - 1);
    createMap(e, mapIdx);

    mapBounds bounds = {.min = {.x = FLT_MAX, .y = FLT_MAX}, .max = {.x = FLT_MIN, .y = FLT_MIN}};
    for (size_t i = 0; i < cc_array_size(e->walls); i++) {
        const wallEntity *wall = safe_array_get_at(e->walls, i);
        bounds.min.x = fminf(wall->pos.pos.x - wall->extent.x + WALL_THICKNESS, bounds.min.x);
        bounds.min.y = fminf(wall->pos.pos.y - wall->extent.y + WALL_THICKNESS, bounds.min.y);
        bounds.max.x = fmaxf(wall->pos.pos.x + wall->extent.x - WALL_THICKNESS, bounds.max.x);
        bounds.max.y = fmaxf(wall->pos.pos.y + wall->extent.y - WALL_THICKNESS, bounds.max.y);
    }
    e->bounds = bounds;

    DEBUG_LOG("creating drones");
    for (uint8_t i = 0; i < e->numDrones; i++) {
        createDrone(e, i);
    }

    DEBUG_LOG("placing floating walls");
    placeRandFloatingWalls(e, mapIdx);

    DEBUG_LOG("creating weapon pickups");
    for (uint8_t i = 0; i < maps[mapIdx]->weaponPickups; i++) {
        createWeaponPickup(e);
    }

    if (e->client != NULL) {
        renderEnv(e);
    }

    computeObs(e);
}

env *initEnv(env *e, uint8_t numDrones, uint8_t numAgents, uint8_t *obs, bool discretizeActions, float *contActions, int32_t *discActions, float *rewards, uint8_t *terminals, uint8_t *truncations, logBuffer *logs, uint64_t seed, bool isTraining) {
    e->numDrones = numDrones;
    e->numAgents = numAgents;
    e->isTraining = isTraining;

    e->obsBytes = obsBytes(e->numDrones);
    e->mapObsBytes = alignedSize(MAP_OBS_SIZE * sizeof(uint8_t), sizeof(float));

    e->obs = obs;
    e->discretizeActions = discretizeActions;
    e->contActions = contActions;
    e->discActions = discActions;
    e->rewards = rewards;
    e->terminals = terminals;
    e->truncations = truncations;

    e->randState = seed;
    e->needsReset = false;

    e->logs = logs;

    cc_array_new(&e->cells);
    cc_array_new(&e->walls);
    e->wallTree = kd_create(2);
    cc_array_new(&e->floatingWalls);
    cc_array_new(&e->drones);
    cc_array_new(&e->pickups);
    // e->pickupTree = kd_create(2);
    cc_slist_new(&e->projectiles);

    e->humanInput = false;
    e->humanDroneInput = 0;
    if (e->numAgents != e->numDrones) {
        e->humanDroneInput = e->numAgents;
    }

    setupEnv(e);

    return e;
}

void clearEnv(env *e) {
    // rewards get cleared in stepEnv every step
    memset(e->terminals, 0x0, e->numAgents * sizeof(uint8_t));
    memset(e->truncations, 0x0, e->numAgents * sizeof(uint8_t));

    e->episodeLength = 0;
    memset(e->stats, 0x0, sizeof(e->stats));

    for (size_t i = 0; i < cc_array_size(e->pickups); i++) {
        weaponPickupEntity *pickup = safe_array_get_at(e->pickups, i);
        destroyWeaponPickup(e, pickup, false);
    }

    for (uint8_t i = 0; i < e->numDrones; i++) {
        droneEntity *drone = safe_array_get_at(e->drones, i);
        destroyDrone(drone);
    }

    destroyAllProjectiles(e);

    for (size_t i = 0; i < cc_array_size(e->walls); i++) {
        wallEntity *wall = safe_array_get_at(e->walls, i);
        destroyWall(wall);
    }

    for (size_t i = 0; i < cc_array_size(e->floatingWalls); i++) {
        wallEntity *wall = safe_array_get_at(e->floatingWalls, i);
        destroyWall(wall);
    }

    for (size_t i = 0; i < cc_array_size(e->cells); i++) {
        mapCell *cell = safe_array_get_at(e->cells, i);
        fastFree(cell);
    }

    cc_array_remove_all(e->cells);
    cc_array_remove_all(e->walls);
    kd_clear(e->wallTree);
    cc_array_remove_all(e->floatingWalls);
    cc_array_remove_all(e->drones);
    cc_array_remove_all(e->pickups);
    // kd_clear(e->pickupTree);
    cc_slist_remove_all(e->projectiles);

    b2DestroyWorld(e->worldID);
}

void destroyEnv(env *e) {
    clearEnv(e);

    cc_array_destroy(e->cells);
    cc_array_destroy(e->walls);
    free(e->wallTree);
    cc_array_destroy(e->floatingWalls);
    cc_array_destroy(e->drones);
    cc_array_destroy(e->pickups);
    free(e->pickupTree);
    cc_slist_destroy(e->projectiles);
}

void resetEnv(env *e) {
    clearEnv(e);
    setupEnv(e);
}

float computeShotHitReward(env *e, const uint8_t enemyIdx) {
    // compute reward based off of how much the projectile(s) or explosion(s)
    // caused the enemy drone to change velocity
    const droneEntity *enemyDrone = safe_array_get_at(e->drones, enemyIdx);
    const float prevEnemySpeed = b2Length(enemyDrone->lastVelocity);
    const float curEnemySpeed = b2Length(b2Body_GetLinearVelocity(enemyDrone->bodyID));
    return scaleValue(fabsf(curEnemySpeed - prevEnemySpeed), MAX_SPEED, true) * SHOT_HIT_REWARD_COEF;
}

// TODO: add death punishment when there are more than 2 drones
float computeReward(env *e, droneEntity *drone) {
    float reward = 0.0f;

    bool aimingAtEnemy = false;
    for (uint8_t i = 0; i < e->numDrones; i++) {
        if (i == drone->idx) {
            continue;
        }
        if (drone->stepInfo.pickedUpWeapon && drone->stepInfo.prevWeapon == STANDARD_WEAPON) {
            reward += WEAPON_PICKUP_REWARD;
        }
        if (drone->stepInfo.shotHit[i] || drone->stepInfo.explosionHit[i]) {
            reward += SHOT_HIT_REWARD;
            //reward += computeShotHitReward(e, i);
        }

        const droneEntity *enemyDrone = safe_array_get_at(e->drones, i);
        const b2Vec2 enemyDirection = b2Normalize(b2Sub(enemyDrone->pos.pos, drone->pos.pos));
        const float velocityToEnemy = b2Dot(drone->lastVelocity, enemyDirection);
        const float enemyDistance = b2Distance(enemyDrone->pos.pos, drone->pos.pos);
        // stop rewarding approaching an enemy if they're very close
        // to avoid constant clashing; always reward approaching when
        // the current weapon is the shotgun, it greatly benefits from
        // being close to enemies
        if (velocityToEnemy > 0.1f && (drone->weaponInfo->type == SHOTGUN_WEAPON || enemyDistance > DISTANCE_CUTOFF)) {
            reward += APPROACH_REWARD_COEF * velocityToEnemy;
        }

        // if we know this drone is aiming at another drone or doesn't
        /// have the enemy drone in the line of sight, then we
        // don't need to check if it's aiming at the enemy drone
        if (aimingAtEnemy || !drone->inLineOfSight[i]) {
            continue;
        }

        const float aimDot = b2Dot(drone->lastAim, enemyDirection);
        const float distance = b2Distance(drone->pos.pos, enemyDrone->pos.pos);
        const float aimThreshold = cosf(atanf(AIM_TOLERANCE / distance));
        if (aimDot >= aimThreshold) {
            reward += AIM_REWARD;
            if (drone->stepInfo.firedShot) {
                reward += AIMED_SHOT_REWARD;
            }
            aimingAtEnemy = true;
        }
    }

    return reward;
}

const float REWARD_EPS = 1.0e-6f;

void computeRewards(env *e, const bool roundOver, const int8_t winner) {
    float rewards[e->numDrones];
    memset(rewards, 0.0f, e->numDrones * sizeof(float));

    if (roundOver && winner != -1) {
        e->rewards[winner] += WIN_REWARD;
    }

    for (uint8_t i = 0; i < e->numAgents; i++) {
        droneEntity *drone = safe_array_get_at(e->drones, i);
        e->rewards[i] += computeReward(e, drone);
    }

    // don't zero sum rewards if there's only one agent
    if (e->numAgents == 1) {
        for (uint8_t i = 0; i < e->numDrones; i++) {
            e->rewards[i] += rewards[i];
            e->stats[i].reward += rewards[i];
        }
        return;
    }
}

static inline bool isActionNoop(const b2Vec2 action) {
    return b2Length(action) < ACTION_NOOP_MAGNITUDE;
}

agentActions _computeActions(env *e, droneEntity *drone, const agentActions *manualActions) {
    agentActions actions = {0};

    if (e->discretizeActions && manualActions == NULL) {
        const uint8_t offset = drone->idx * DISCRETE_ACTION_SIZE;
        const uint8_t move = e->discActions[offset + 0];
        // 8 is no-op for both move and aim
        ASSERT(move <= 8);
        if (move != 8) {
            actions.move.x = discToContActionMap[0][move];
            actions.move.y = discToContActionMap[1][move];
        }
        const uint8_t aim = e->discActions[offset + 1];
        ASSERT(move <= 8);
        if (aim != 8) {
            actions.aim.x = discToContActionMap[0][aim];
            actions.aim.y = discToContActionMap[1][aim];
        }
        const uint8_t shoot = e->discActions[offset + 2];
        ASSERT(shoot <= 1);
        actions.shoot = (bool)shoot;
        return actions;
    }

    const uint8_t offset = drone->idx * CONTINUOUS_ACTION_SIZE;
    if (manualActions == NULL) {
        actions.move = (b2Vec2){.x = tanhf(e->contActions[offset + 0]), .y = tanhf(e->contActions[offset + 1])};
        actions.aim = (b2Vec2){.x = tanhf(e->contActions[offset + 2]), .y = tanhf(e->contActions[offset + 3])};
        actions.shoot = (bool)e->contActions[offset + 4];
    } else {
        actions.move = manualActions->move;
        actions.aim = manualActions->aim;
        actions.shoot = manualActions->shoot;
    }

    ASSERT_VEC_BOUNDED(actions.move);
    // cap movement magnitude to 1.0
    if (b2Length(actions.move) > 1.0f) {
        actions.move = b2Normalize(actions.move);
    } else if (isActionNoop(actions.move)) {
        actions.move = b2Vec2_zero;
    }

    ASSERT_VEC_BOUNDED(actions.aim);
    if (isActionNoop(actions.aim)) {
        actions.aim = b2Vec2_zero;
    } else {
        actions.aim = b2Normalize(actions.aim);
    }

    return actions;
}

agentActions computeActions(env *e, droneEntity *drone, const agentActions *manualActions) {
    const agentActions actions = _computeActions(e, drone, manualActions);
    drone->lastMove = actions.move;
    if (!b2VecEqual(actions.aim, b2Vec2_zero)) {
        drone->lastAim = actions.aim;
    }
    return actions;
}

void updateHumanInputToggle(env *e) {
    if (IsKeyPressed(KEY_LEFT_CONTROL)) {
        e->humanInput = !e->humanInput;
    }
    if (IsKeyPressed(KEY_ONE) || IsKeyPressed(KEY_KP_1)) {
        e->humanDroneInput = 0;
    }
    if (IsKeyPressed(KEY_TWO) || IsKeyPressed(KEY_KP_2)) {
        e->humanDroneInput = 1;
    }
}

agentActions getPlayerInputs(env *e, droneEntity *drone, uint8_t gamepadIdx) {
    agentActions actions = {0};

    bool controllerConnected = false;
    if (IsGamepadAvailable(gamepadIdx)) {
        controllerConnected = true;
    } else if (IsGamepadAvailable(0)) {
        controllerConnected = true;
        gamepadIdx = 0;
    }
    if (controllerConnected) {
        float lStickX = GetGamepadAxisMovement(gamepadIdx, GAMEPAD_AXIS_LEFT_X);
        float lStickY = GetGamepadAxisMovement(gamepadIdx, GAMEPAD_AXIS_LEFT_Y);
        float rStickX = GetGamepadAxisMovement(gamepadIdx, GAMEPAD_AXIS_RIGHT_X);
        float rStickY = GetGamepadAxisMovement(gamepadIdx, GAMEPAD_AXIS_RIGHT_Y);

        bool shoot = IsGamepadButtonDown(gamepadIdx, GAMEPAD_BUTTON_RIGHT_TRIGGER_2);
        if (!shoot) {
            shoot = IsGamepadButtonDown(gamepadIdx, GAMEPAD_BUTTON_RIGHT_TRIGGER_1);
        }

        actions.move = (b2Vec2){.x = lStickX, .y = lStickY};
        actions.aim = (b2Vec2){.x = rStickX, .y = rStickY};
        actions.shoot = shoot;
        return computeActions(e, drone, &actions);
    }

    b2Vec2 move = b2Vec2_zero;
    if (IsKeyDown(KEY_W)) {
        move.y += -1.0f;
    }
    if (IsKeyDown(KEY_S)) {
        move.y += 1.0f;
    }
    if (IsKeyDown(KEY_A)) {
        move.x += -1.0f;
    }
    if (IsKeyDown(KEY_D)) {
        move.x += 1.0f;
    }
    actions.move = b2Normalize(move);

    Vector2 mousePos = (Vector2){.x = (float)GetMouseX(), .y = (float)GetMouseY()};
    b2Vec2 dronePos = b2Body_GetPosition(drone->bodyID);
    actions.aim = b2Normalize(b2Sub(rayVecToB2Vec(e->client, mousePos), dronePos));

    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        actions.shoot = true;
    }

    return computeActions(e, drone, &actions);
}

void stepEnv(env *e) {
    if (e->needsReset) {
        DEBUG_LOG("Resetting environment");
        resetEnv(e);
    }

    agentActions stepActions[e->numDrones];
    memset(stepActions, 0x0, e->numDrones * sizeof(agentActions));

    // preprocess actions for the next N steps
    for (uint8_t i = 0; i < e->numDrones; i++) {
        droneEntity *drone = safe_array_get_at(e->drones, i);
        if (i < e->numAgents) {
            stepActions[i] = computeActions(e, drone, NULL);
        } else {
            const agentActions botActions = scriptedBotActions(e, drone);
            stepActions[i] = computeActions(e, drone, &botActions);
        }
    }

    // reset reward buffer
    memset(e->rewards, 0x0, e->numAgents * sizeof(float));

    for (int i = 0; i < FRAMESKIP; i++) {
        e->episodeLength++;

        // handle actions
        if (e->client != NULL) {
            updateHumanInputToggle(e);
        }

        for (uint8_t i = 0; i < e->numDrones; i++) {
            droneEntity *drone = safe_array_get_at(e->drones, i);
            drone->lastVelocity = b2Body_GetLinearVelocity(drone->bodyID);
            memset(&drone->stepInfo, 0x0, sizeof(droneStepInfo));
            memset(&drone->inLineOfSight, 0x0, sizeof(drone->inLineOfSight));

            agentActions actions;
            // take inputs from humans every frame
            if (e->humanInput && e->humanDroneInput == i) {
                actions = getPlayerInputs(e, drone, i);
            } else {
                actions = stepActions[i];
            }

            if (!b2VecEqual(actions.move, b2Vec2_zero)) {
                droneMove(drone, actions.move);
            }
            if (actions.shoot) {
                droneShoot(e, drone, actions.aim);
            }
        }

        // update entity info, step physics, and handle events
        b2World_Step(e->worldID, DELTA_TIME, BOX2D_SUBSTEPS);

        // mark old positions as invalid now that physics has been stepped
        // projectiles will have their positions correctly updated in projectilesStep
        for (uint8_t i = 0; i < e->numDrones; i++) {
            droneEntity *drone = safe_array_get_at(e->drones, i);
            drone->pos.valid = false;
        }
        for (size_t i = 0; i < cc_array_size(e->floatingWalls); i++) {
            wallEntity *wall = safe_array_get_at(e->floatingWalls, i);
            wall->pos.valid = false;
        }

        // handle sudden death
        e->stepsLeft = fmaxf(e->stepsLeft - 1, 0.0f);
        if (e->stepsLeft == 0 && e->numAgents > 1) {
            e->suddenDeathSteps = fmaxf(e->suddenDeathSteps - 1, 0.0f);
            if (e->suddenDeathSteps == 0) {
                DEBUG_LOG("placing sudden death walls");
                handleSuddenDeath(e);
                e->suddenDeathSteps = SUDDEN_DEATH_STEPS;
            }
        }

        projectilesStep(e);

        handleContactEvents(e);
        handleSensorEvents(e);

        int8_t lastAlive = -1;
        uint8_t deadDrones = 0;
        for (uint8_t i = 0; i < e->numDrones; i++) {
            droneEntity *drone = safe_array_get_at(e->drones, i);
            droneStep(e, drone, DELTA_TIME);
            if (drone->dead) {
                deadDrones++;
                if (i < e->numAgents) {
                    e->terminals[i] = 1;
                }
            } else {
                lastAlive = i;
            }
        }

        weaponPickupsStep(e, DELTA_TIME);

        bool roundOver = deadDrones >= e->numDrones - 1;
        if (e->numAgents == 1 && e->stepsLeft == 0) {
            roundOver = true;
        }
        computeRewards(e, roundOver, lastAlive);

        if (e->client != NULL) {
            renderEnv(e);
        }

        if (roundOver) {
            if (e->numAgents == 1 && e->stepsLeft == 0) {
                DEBUG_LOG("truncating episode");
                memset(e->truncations, 1, e->numAgents * sizeof(uint8_t));
            } else {
                DEBUG_LOG("terminating episode");
                memset(e->terminals, 1, e->numAgents * sizeof(uint8_t));
            }

            if (lastAlive != -1) {
                e->stats[lastAlive].wins = 1.0f;
            }

            // set absolute distance traveled of agent drones
            for (uint8_t i = 0; i < e->numDrones; i++) {
                const droneEntity *drone = safe_array_get_at(e->drones, i);
                e->stats[i].absDistanceTraveled = b2Distance(drone->initalPos, drone->pos.pos);
            }

            // add existing projectile distances to stats
            for (SNode *cur = e->projectiles->head; cur != NULL; cur = cur->next) {
                const projectileEntity *projectile = (projectileEntity *)cur->data;
                e->stats[projectile->droneIdx].shotDistances[projectile->weaponInfo->type] += projectile->distance;
            }

            logEntry log = {0};
            log.length = e->episodeLength;
            memcpy(log.stats, e->stats, sizeof(e->stats));
            addLogEntry(e->logs, &log);

            e->needsReset = true;
            break;
        }
    }

    for (uint8_t i = 0; i < e->numAgents; i++) {
        const float reward = e->rewards[i];
        if (reward > REWARD_EPS || reward < -REWARD_EPS) {
            DEBUG_LOGF("step: %f drone: %d reward: %f", ROUND_STEPS - e->stepsLeft, i, reward);
        }
    }

    computeObs(e);
}

bool envTerminated(env *e) {
    for (uint8_t i = 0; i < e->numDrones; i++) {
        droneEntity *drone = safe_array_get_at(e->drones, i);
        if (drone->dead) {
            return true;
        }
    }
    return false;
}

#endif
