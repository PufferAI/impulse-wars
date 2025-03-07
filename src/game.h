#ifndef IMPULSE_WARS_GAME_H
#define IMPULSE_WARS_GAME_H

#include "env.h"
#include "helpers.h"
#include "settings.h"
#include "types.h"

// these functions call each other so need to be forward declared
void destroyProjectile(env *e, projectileEntity *projectile, const bool processExplosions, const bool full);
void createExplosion(env *e, droneEntity *drone, const projectileEntity *projectile, const b2ExplosionDef *def);

void updateTrailPoints(const env *e, trailPoints *tp, const uint8_t maxLen, const b2Vec2 pos);

static inline bool entityTypeIsWall(const enum entityType type) {
    // walls are the first 3 entity types
    return type <= DEATH_WALL_ENTITY;
}

static inline int16_t cellIndex(const env *e, const int8_t col, const int8_t row) {
    return col + (row * e->map->columns);
}

// discretizes an entity's position into a cell index; -1 is returned if
// the position is out of bounds of the map
static inline int16_t entityPosToCellIdx(const env *e, const b2Vec2 pos) {
    const float cellX = pos.x + (((float)e->map->columns * WALL_THICKNESS) / 2.0f);
    const float cellY = pos.y + (((float)e->map->rows * WALL_THICKNESS) / 2.0f);
    const int8_t cellCol = cellX / WALL_THICKNESS;
    const int8_t cellRow = cellY / WALL_THICKNESS;
    const int16_t cellIdx = cellIndex(e, cellCol, cellRow);
    // set the cell to -1 if it's out of bounds
    if (cellIdx < 0 || (uint16_t)cellIdx >= cc_array_size(e->cells)) {
        DEBUG_LOGF("invalid cell index: %d from position: (%f, %f)", cellIdx, pos.x, pos.y);
        return -1;
    }
    return cellIdx;
}

typedef struct overlapAABBCtx {
    bool overlaps;
} overlapAABBCtx;

bool overlapAABBCallback(b2ShapeId shapeID, void *context) {
    if (!b2Shape_IsValid(shapeID)) {
        return true;
    }

    overlapAABBCtx *ctx = context;
    ctx->overlaps = true;
    return true;
}

// returns true if the given position overlaps with shapes in a bounding
// box with a height and width of distance
bool isOverlappingAABB(const env *e, const b2Vec2 pos, const float distance, const b2QueryFilter filter) {
    b2AABB bounds = {
        .lowerBound = {.x = pos.x - distance, .y = pos.y - distance},
        .upperBound = {.x = pos.x + distance, .y = pos.y + distance},
    };
    overlapAABBCtx ctx = {.overlaps = false};
    b2World_OverlapAABB(e->worldID, bounds, filter, overlapAABBCallback, &ctx);
    return ctx.overlaps;
}

b2ShapeProxy makeDistanceProxyFromType(const enum entityType type, bool *isCircle) {
    b2ShapeProxy proxy = {0};
    switch (type) {
    case DRONE_ENTITY:
        *isCircle = true;
        proxy.radius = DRONE_RADIUS;
        break;
    case SHIELD_ENTITY:
        *isCircle = true;
        proxy.radius = DRONE_SHIELD_RADIUS;
        break;
    case WEAPON_PICKUP_ENTITY:
        proxy.count = 4;
        proxy.points[0] = (b2Vec2){.x = -PICKUP_THICKNESS / 2.0f, .y = -PICKUP_THICKNESS / 2.0f};
        proxy.points[1] = (b2Vec2){.x = -PICKUP_THICKNESS / 2.0f, .y = +PICKUP_THICKNESS / 2.0f};
        proxy.points[2] = (b2Vec2){.x = +PICKUP_THICKNESS / 2.0f, .y = -PICKUP_THICKNESS / 2.0f};
        proxy.points[3] = (b2Vec2){.x = +PICKUP_THICKNESS / 2.0f, .y = +PICKUP_THICKNESS / 2.0f};
        break;
    case STANDARD_WALL_ENTITY:
    case BOUNCY_WALL_ENTITY:
    case DEATH_WALL_ENTITY:
        proxy.count = 4;
        proxy.points[0] = (b2Vec2){.x = -FLOATING_WALL_THICKNESS / 2.0f, .y = -FLOATING_WALL_THICKNESS / 2.0f};
        proxy.points[1] = (b2Vec2){.x = -FLOATING_WALL_THICKNESS / 2.0f, .y = +FLOATING_WALL_THICKNESS / 2.0f};
        proxy.points[2] = (b2Vec2){.x = +FLOATING_WALL_THICKNESS / 2.0f, .y = -FLOATING_WALL_THICKNESS / 2.0f};
        proxy.points[3] = (b2Vec2){.x = +FLOATING_WALL_THICKNESS / 2.0f, .y = +FLOATING_WALL_THICKNESS / 2.0f};
        break;
    default:
        ERRORF("unknown entity type for shape distance: %d", type);
    }

    return proxy;
}

b2ShapeProxy makeDistanceProxy(const entity *ent, bool *isCircle) {
    if (ent->type == PROJECTILE_ENTITY) {
        *isCircle = true;
        b2ShapeProxy proxy = {0};
        const projectileEntity *proj = ent->entity;
        proxy.radius = proj->weaponInfo->radius;
        return proxy;
    }

    return makeDistanceProxyFromType(ent->type, isCircle);
}

b2Transform entityTransform(const entity *ent) {
    b2Transform transform;
    wallEntity *wall;
    projectileEntity *proj;
    droneEntity *drone;

    switch (ent->type) {
    case STANDARD_WALL_ENTITY:
    case BOUNCY_WALL_ENTITY:
    case DEATH_WALL_ENTITY:
        wall = ent->entity;
        transform.p = wall->pos;
        transform.q = wall->rot;
        return transform;
    case PROJECTILE_ENTITY:
        proj = ent->entity;
        transform.p = proj->pos;
        return transform;
    case DRONE_ENTITY:
        drone = ent->entity;
        transform.p = drone->pos;
        return transform;
    default:
        ERRORF("unknown entity type: %d", ent->type);
    }
}

// returns the closest points between two entities
b2DistanceOutput closestPoint(const entity *srcEnt, const entity *dstEnt) {
    bool isCircle = false;
    b2DistanceInput input;
    input.proxyA = makeDistanceProxy(srcEnt, &isCircle);
    input.proxyB = makeDistanceProxy(dstEnt, &isCircle);
    input.transformA = entityTransform(srcEnt);
    input.transformB = entityTransform(dstEnt);
    input.useRadii = isCircle;

    b2SimplexCache cache = {0};
    return b2ShapeDistance(&cache, &input, NULL, 0);
}

typedef struct behindWallContext {
    const entity *srcEnt;
    const enum entityType *targetType;
    bool hit;
} behindWallContext;

float posBehindWallCallback(b2ShapeId shapeID, b2Vec2 point, b2Vec2 normal, float fraction, void *context) {
    // these are unused but required by the b2CastResultFcn callback prototype
    MAYBE_UNUSED(point);
    MAYBE_UNUSED(normal);
    MAYBE_UNUSED(fraction);

    behindWallContext *ctx = context;
    const entity *ent = b2Shape_GetUserData(shapeID);
    if (ent == ctx->srcEnt || (ctx->targetType != NULL && ent->type == *ctx->targetType)) {
        return -1;
    }
    ctx->hit = true;
    return 0;
}

// returns true if there are shapes that match filter between startPos and endPos
bool posBehindWall(const env *e, const b2Vec2 startPos, const b2Vec2 endPos, const entity *srcEnt, const b2QueryFilter filter, const enum entityType *targetType) {
    const float rayDistance = b2Distance(startPos, endPos);
    // if the two points are extremely close we can safely assume the
    // entity isn't behind a wall
    if (rayDistance <= 1.0f) {
        return false;
    }

    const b2Vec2 translation = b2Sub(endPos, startPos);
    behindWallContext ctx = {
        .srcEnt = srcEnt,
        .targetType = targetType,
        .hit = false,
    };
    b2World_CastRay(e->worldID, startPos, translation, filter, posBehindWallCallback, &ctx);
    return ctx.hit;
}

typedef struct overlapCircleCtx {
    const env *e;
    const entity *ent;
    const enum entityType *targetType;
    const b2QueryFilter filter;
    bool overlaps;
} overlapCircleCtx;

bool isOverlappingCircleCallback(b2ShapeId shapeID, void *context) {
    if (!b2Shape_IsValid(shapeID)) {
        return true;
    }

    overlapCircleCtx *ctx = context;
    const entity *overlappingEnt = b2Shape_GetUserData(shapeID);
    if (ctx->targetType != NULL && overlappingEnt->type != *ctx->targetType) {
        return true;
    }

    const b2DistanceOutput output = closestPoint(ctx->ent, overlappingEnt);
    const bool behind = posBehindWall(ctx->e, output.pointA, output.pointB, ctx->ent, ctx->filter, ctx->targetType);
    if (!behind) {
        ctx->overlaps = true;
    } else if (ctx->ent->type == PROJECTILE_ENTITY) {
        projectileEntity *proj = ctx->ent->entity;
        droneEntity *drone = overlappingEnt->entity;
        proj->dronesBehindWalls[proj->numDronesBehindWalls++] = drone->idx;
    }
    return behind;
}

bool isOverlappingCircleInLineOfSight(const env *e, const entity *ent, const b2Vec2 startPos, const float radius, const b2QueryFilter filter, const enum entityType *targetType) {
    const b2Circle circle = {.center = b2Vec2_zero, .radius = radius};
    const b2Transform transform = {.p = startPos, .q = b2Rot_identity};
    overlapCircleCtx ctx = {
        .e = e,
        .ent = ent,
        .targetType = targetType,
        .filter = (b2QueryFilter){
            .categoryBits = filter.categoryBits,
            .maskBits = WALL_SHAPE | FLOATING_WALL_SHAPE,
        },
        .overlaps = false,
    };
    b2World_OverlapCircle(e->worldID, &circle, transform, filter, isOverlappingCircleCallback, &ctx);
    return ctx.overlaps;
}

// returns true and sets emptyPos to the position of an empty cell
// that is an appropriate distance away from other entities if one exists;
// if quad is set to -1 a random valid position from anywhere on the map
// will be returned, otherwise a position within the specified quadrant
// will be returned
bool findOpenPos(env *e, const enum shapeCategory shapeType, b2Vec2 *emptyPos, int8_t quad) {
    uint8_t checkedCells[BITNSLOTS(MAX_CELLS)] = {0};
    const size_t nCells = cc_array_size(e->cells);
    uint16_t attempts = 0;

    while (true) {
        if (attempts == nCells) {
            return false;
        }

        uint16_t cellIdx;
        if (quad == -1) {
            cellIdx = randInt(&e->randState, 0, nCells - 1);
        } else {
            const float minX = e->map->spawnQuads[quad].min.x;
            const float minY = e->map->spawnQuads[quad].min.y;
            const float maxX = e->map->spawnQuads[quad].max.x;
            const float maxY = e->map->spawnQuads[quad].max.y;

            b2Vec2 randPos = {.x = randFloat(&e->randState, minX, maxX), .y = randFloat(&e->randState, minY, maxY)};
            cellIdx = entityPosToCellIdx(e, randPos);
        }
        if (bitTest(checkedCells, cellIdx)) {
            continue;
        }
        bitSet(checkedCells, cellIdx);
        attempts++;

        const mapCell *cell = safe_array_get_at(e->cells, cellIdx);
        if (cell->ent != NULL) {
            continue;
        }

        if (shapeType == WEAPON_PICKUP_SHAPE) {
            // ensure pickups don't spawn too close to other pickups
            bool tooClose = false;
            for (uint8_t i = 0; i < cc_array_size(e->pickups); i++) {
                const weaponPickupEntity *pickup = safe_array_get_at(e->pickups, i);
                if (b2DistanceSquared(cell->pos, pickup->pos) < PICKUP_SPAWN_DISTANCE_SQUARED) {
                    tooClose = true;
                    break;
                }
            }
            if (tooClose) {
                continue;
            }
        } else if (shapeType == DRONE_SHAPE) {
            if (!e->map->droneSpawns[cellIdx]) {
                continue;
            }

            // ensure drones don't spawn too close to other drones
            bool tooClose = false;
            for (uint8_t i = 0; i < cc_array_size(e->drones); i++) {
                const droneEntity *drone = safe_array_get_at(e->drones, i);
                if (b2DistanceSquared(cell->pos, drone->pos) < DRONE_DRONE_SPAWN_DISTANCE_SQUARED) {
                    tooClose = true;
                    break;
                }
            }
            if (tooClose) {
                continue;
            }
        }

        const b2QueryFilter filter = {
            .categoryBits = shapeType,
            .maskBits = FLOATING_WALL_SHAPE | DRONE_SHAPE,
        };
        if (!isOverlappingAABB(e, cell->pos, MIN_SPAWN_DISTANCE, filter)) {
            *emptyPos = cell->pos;
            return true;
        }
    }
}

entity *createWall(const env *e, const b2Vec2 pos, const float width, const float height, int16_t cellIdx, const enum entityType type, const bool floating) {
    ASSERT(cellIdx != -1);
    ASSERT(entityTypeIsWall(type));

    b2BodyDef wallBodyDef = b2DefaultBodyDef();
    wallBodyDef.position = pos;
    if (floating) {
        wallBodyDef.type = b2_dynamicBody;
        wallBodyDef.linearDamping = FLOATING_WALL_DAMPING;
        wallBodyDef.angularDamping = FLOATING_WALL_DAMPING;
        wallBodyDef.isAwake = false;
    }
    b2BodyId wallBodyID = b2CreateBody(e->worldID, &wallBodyDef);

    b2Vec2 extent = {.x = width / 2.0f, .y = height / 2.0f};
    b2ShapeDef wallShapeDef = b2DefaultShapeDef();
    wallShapeDef.density = WALL_DENSITY;
    wallShapeDef.restitution = STANDARD_WALL_RESTITUTION;
    wallShapeDef.friction = STANDARD_WALL_FRICTION;
    wallShapeDef.filter.categoryBits = WALL_SHAPE;
    wallShapeDef.filter.maskBits = FLOATING_WALL_SHAPE | PROJECTILE_SHAPE | DRONE_SHAPE | SHIELD_SHAPE;
    if (floating) {
        wallShapeDef.filter.categoryBits = FLOATING_WALL_SHAPE;
        wallShapeDef.filter.maskBits |= WALL_SHAPE | WEAPON_PICKUP_SHAPE;
        wallShapeDef.enableSensorEvents = true;
    }

    if (type == BOUNCY_WALL_ENTITY) {
        wallShapeDef.restitution = BOUNCY_WALL_RESTITUTION;
        wallShapeDef.friction = 0.0f;
    } else if (type == DEATH_WALL_ENTITY) {
        wallShapeDef.enableContactEvents = true;
    }

    wallEntity *wall = fastCalloc(1, sizeof(wallEntity));
    wall->bodyID = wallBodyID;
    wall->pos = pos;
    wall->rot = b2Rot_identity;
    wall->velocity = b2Vec2_zero;
    wall->extent = extent;
    wall->mapCellIdx = cellIdx;
    wall->isFloating = floating;
    wall->type = type;
    wall->isSuddenDeath = e->suddenDeathWallsPlaced;

    entity *ent = fastCalloc(1, sizeof(entity));
    ent->type = type;
    ent->entity = wall;

    wall->ent = ent;
    wallShapeDef.userData = ent;
    const b2Polygon wallPolygon = b2MakeBox(extent.x, extent.y);
    wall->shapeID = b2CreatePolygonShape(wallBodyID, &wallShapeDef, &wallPolygon);
    b2Body_SetUserData(wall->bodyID, ent);

    if (floating) {
        cc_array_add(e->floatingWalls, wall);
    } else {
        cc_array_add(e->walls, wall);
    }

    return ent;
}

void destroyWall(const env *e, wallEntity *wall, const bool full) {
    fastFree(wall->ent);

    if (full) {
        mapCell *cell = safe_array_get_at(e->cells, wall->mapCellIdx);
        cell->ent = NULL;
    }

    b2DestroyBody(wall->bodyID);
    fastFree(wall);
}

enum weaponType randWeaponPickupType(env *e) {
    // spawn weapon pickups according to their spawn weights and how many
    // pickups are currently spawned with different weapons
    float totalWeight = 0.0f;
    float spawnWeights[_NUM_WEAPONS - 1] = {0};
    for (uint8_t i = 1; i < NUM_WEAPONS; i++) {
        if (i == e->defaultWeapon->type) {
            continue;
        }
        spawnWeights[i - 1] = weaponInfos[i]->spawnWeight / ((e->spawnedWeaponPickups[i] + 1) * 2.0f);
        totalWeight += spawnWeights[i - 1];
    }

    const float randPick = randFloat(&e->randState, 0.0f, totalWeight);
    float cumulativeWeight = 0.0f;
    enum weaponType type = STANDARD_WEAPON;
    for (uint8_t i = 1; i < NUM_WEAPONS; i++) {
        if (i == e->defaultWeapon->type) {
            continue;
        }
        cumulativeWeight += spawnWeights[i - 1];
        if (randPick < cumulativeWeight) {
            type = i;
            break;
        }
    }
    ASSERT(type != STANDARD_WEAPON && type != e->defaultWeapon->type);
    e->spawnedWeaponPickups[type]++;

    return type;
}

void createWeaponPickupBodyShape(const env *e, weaponPickupEntity *pickup) {
    pickup->bodyDestroyed = false;

    b2BodyDef pickupBodyDef = b2DefaultBodyDef();
    pickupBodyDef.position = pickup->pos;
    pickupBodyDef.userData = pickup->ent;
    pickup->bodyID = b2CreateBody(e->worldID, &pickupBodyDef);

    b2ShapeDef pickupShapeDef = b2DefaultShapeDef();
    pickupShapeDef.filter.categoryBits = WEAPON_PICKUP_SHAPE;
    pickupShapeDef.filter.maskBits = FLOATING_WALL_SHAPE | DRONE_SHAPE;
    pickupShapeDef.isSensor = true;
    pickupShapeDef.enableSensorEvents = true;
    pickupShapeDef.userData = pickup->ent;
    const b2Polygon pickupPolygon = b2MakeBox(PICKUP_THICKNESS / 2.0f, PICKUP_THICKNESS / 2.0f);
    pickup->shapeID = b2CreatePolygonShape(pickup->bodyID, &pickupShapeDef, &pickupPolygon);
}

void createWeaponPickup(env *e) {
    // ensure weapon pickups are initially spawned somewhat uniformly
    b2Vec2 pos;
    e->lastSpawnQuad = (e->lastSpawnQuad + 1) % 4;
    if (!findOpenPos(e, WEAPON_PICKUP_SHAPE, &pos, e->lastSpawnQuad)) {
        ERROR("no open position for weapon pickup");
    }

    weaponPickupEntity *pickup = fastCalloc(1, sizeof(weaponPickupEntity));
    pickup->weapon = randWeaponPickupType(e);
    pickup->respawnWait = 0.0f;
    pickup->floatingWallsTouching = 0;
    pickup->pos = pos;

    entity *ent = fastCalloc(1, sizeof(entity));
    ent->type = WEAPON_PICKUP_ENTITY;
    ent->entity = pickup;
    pickup->ent = ent;

    const int16_t cellIdx = entityPosToCellIdx(e, pos);
    if (cellIdx == -1) {
        ERRORF("invalid position for weapon pickup spawn: (%f, %f)", pos.x, pos.y);
    }
    pickup->mapCellIdx = cellIdx;
    mapCell *cell = safe_array_get_at(e->cells, cellIdx);
    cell->ent = ent;

    createWeaponPickupBodyShape(e, pickup);

    cc_array_add(e->pickups, pickup);
}

void destroyWeaponPickup(const env *e, weaponPickupEntity *pickup) {
    fastFree(pickup->ent);

    mapCell *cell = safe_array_get_at(e->cells, pickup->mapCellIdx);
    cell->ent = NULL;

    if (!pickup->bodyDestroyed) {
        b2DestroyBody(pickup->bodyID);
    }

    fastFree(pickup);
}

// destroys the pickup body and shape while the pickup is waiting to
// respawn to avoid spurious sensor overlap checks; enabling/disabling
// a body is almost as expensive and creating a new body in box2d, and
// manually moving (teleporting) it is expensive as well so destroying
// the body now and re-creating it later is the fastest
void disableWeaponPickup(env *e, weaponPickupEntity *pickup) {
    DEBUG_LOGF("disabling weapon pickup at cell %d (%f, %f)", pickup->mapCellIdx, pickup->pos.x, pickup->pos.y);

    pickup->respawnWait = PICKUP_RESPAWN_WAIT;
    if (e->suddenDeathWallsPlaced) {
        pickup->respawnWait = SUDDEN_DEATH_PICKUP_RESPAWN_WAIT;
    }
    b2DestroyBody(pickup->bodyID);
    pickup->bodyDestroyed = true;

    mapCell *cell = safe_array_get_at(e->cells, pickup->mapCellIdx);
    ASSERT(cell->ent != NULL);
    cell->ent = NULL;

    e->spawnedWeaponPickups[pickup->weapon]--;
}

void createDroneShield(const env *e, droneEntity *drone, const int8_t groupIdx) {
    // the shield is comprised of 2 shapes over 2 bodies:
    // 1. a kinematic body that allows the parent drone to be unaffected
    // by collisions since kinematic bodies have essentially infinite mass
    // 2. a shape on the drone body that collides with walls and other
    // shields, since kinematic bodies don't collide with other kinematic
    // bodies or static bodies

    b2BodyDef shieldBodyDef = b2DefaultBodyDef();
    shieldBodyDef.type = b2_kinematicBody;
    shieldBodyDef.fixedRotation = true;
    shieldBodyDef.position = drone->pos;
    b2BodyId shieldBodyID = b2CreateBody(e->worldID, &shieldBodyDef);

    b2ShapeDef shieldShapeDef = b2DefaultShapeDef();
    shieldShapeDef.filter.categoryBits = SHIELD_SHAPE;
    shieldShapeDef.filter.maskBits = PROJECTILE_SHAPE | DRONE_SHAPE;
    shieldShapeDef.filter.groupIndex = groupIdx;
    shieldShapeDef.enableContactEvents = true;

    b2ShapeDef shieldBufferShapeDef = b2DefaultShapeDef();
    shieldBufferShapeDef.density = 0.0f;
    shieldBufferShapeDef.friction = DRONE_FRICTION;
    shieldBufferShapeDef.restitution = DRONE_RESTITUTION;
    shieldBufferShapeDef.filter.categoryBits = SHIELD_SHAPE;
    shieldBufferShapeDef.filter.maskBits = WALL_SHAPE | FLOATING_WALL_SHAPE | SHIELD_SHAPE;
    shieldBufferShapeDef.filter.groupIndex = groupIdx;

    shieldEntity *shield = fastCalloc(1, sizeof(shieldEntity));
    shield->drone = drone;
    shield->bodyID = shieldBodyID;
    shield->pos = drone->pos;
    shield->health = DRONE_SHIELD_HEALTH;
    shield->duration = DRONE_SHIELD_START_DURATION;

    entity *shieldEnt = fastCalloc(1, sizeof(entity));
    shieldEnt->type = SHIELD_ENTITY;
    shieldEnt->entity = shield;

    const b2Circle shieldCircle = {.center = b2Vec2_zero, .radius = DRONE_SHIELD_RADIUS};

    shield->ent = shieldEnt;
    shieldShapeDef.userData = shieldEnt;
    shield->shapeID = b2CreateCircleShape(shieldBodyID, &shieldShapeDef, &shieldCircle);
    b2Body_SetUserData(shield->bodyID, shieldEnt);

    shieldBufferShapeDef.userData = shieldEnt;
    shield->bufferShapeID = b2CreateCircleShape(drone->bodyID, &shieldBufferShapeDef, &shieldCircle);

    drone->shield = shield;
}

void createDrone(env *e, const uint8_t idx) {
    const int8_t groupIdx = -(idx + 1);
    b2BodyDef droneBodyDef = b2DefaultBodyDef();
    droneBodyDef.type = b2_dynamicBody;

    int8_t spawnQuad = -1;
    if (!e->isTraining) {
        // spawn drones in diagonal quadrants from each other so that
        // they're more likely to be further apart if we're not training;
        // doing this while training will result in much slower learning
        // due to drones starting much farther apart
        if (e->lastSpawnQuad == -1) {
            spawnQuad = randInt(&e->randState, 0, 3);
        } else if (e->numDrones == 2) {
            spawnQuad = 3 - e->lastSpawnQuad;
        } else {
            spawnQuad = (e->lastSpawnQuad + 1) % 4;
        }
        e->lastSpawnQuad = spawnQuad;
    }
    if (!findOpenPos(e, DRONE_SHAPE, &droneBodyDef.position, spawnQuad)) {
        ERROR("no open position for drone");
    }

    droneBodyDef.fixedRotation = true;
    droneBodyDef.linearDamping = DRONE_LINEAR_DAMPING;
    b2BodyId droneBodyID = b2CreateBody(e->worldID, &droneBodyDef);
    b2ShapeDef droneShapeDef = b2DefaultShapeDef();
    droneShapeDef.density = DRONE_DENSITY;
    droneShapeDef.friction = DRONE_FRICTION;
    droneShapeDef.restitution = DRONE_RESTITUTION;
    droneShapeDef.filter.categoryBits = DRONE_SHAPE;
    droneShapeDef.filter.maskBits = WALL_SHAPE | FLOATING_WALL_SHAPE | WEAPON_PICKUP_SHAPE | PROJECTILE_SHAPE | DRONE_SHAPE | SHIELD_SHAPE;
    droneShapeDef.filter.groupIndex = groupIdx;
    droneShapeDef.enableContactEvents = true;
    droneShapeDef.enableSensorEvents = true;
    const b2Circle droneCircle = {.center = b2Vec2_zero, .radius = DRONE_RADIUS};

    droneEntity *drone = fastCalloc(1, sizeof(droneEntity));
    drone->bodyID = droneBodyID;
    drone->weaponInfo = e->defaultWeapon;
    drone->ammo = weaponAmmo(e->defaultWeapon->type, drone->weaponInfo->type);
    drone->energyLeft = DRONE_ENERGY_MAX;
    drone->idx = idx;
    drone->team = idx;
    if (e->teamsEnabled) {
        drone->team = idx / (e->numDrones / 2);
    }
    drone->initalPos = droneBodyDef.position;
    drone->pos = droneBodyDef.position;
    drone->mapCellIdx = entityPosToCellIdx(e, droneBodyDef.position);
    drone->lastAim = (b2Vec2){.x = 0.0f, .y = -1.0f};
    memset(&drone->stepInfo, 0x0, sizeof(droneStepInfo));

    entity *ent = fastCalloc(1, sizeof(entity));
    ent->type = DRONE_ENTITY;
    ent->entity = drone;

    drone->ent = ent;
    droneShapeDef.userData = ent;
    drone->shapeID = b2CreateCircleShape(droneBodyID, &droneShapeDef, &droneCircle);
    b2Body_SetUserData(drone->bodyID, ent);

    cc_array_add(e->drones, drone);

    createDroneShield(e, drone, groupIdx);
}

void destroyDroneShield(shieldEntity *shield) {
    droneEntity *drone = shield->drone;
    drone->shield = NULL;

    b2DestroyBody(shield->bodyID);
    b2DestroyShape(shield->bufferShapeID, false);
    fastFree(shield->ent);
    fastFree(shield);
}

void destroyDrone(droneEntity *drone) {
    fastFree(drone->ent);

    shieldEntity *shield = drone->shield;
    if (shield != NULL) {
        destroyDroneShield(shield);
    }

    b2DestroyBody(drone->bodyID);
    fastFree(drone);
}

void killDrone(env *e, droneEntity *drone) {
    if (drone->dead) {
        return;
    }
    DEBUG_LOGF("drone %d died", drone->idx);

    drone->dead = true;
    drone->diedThisStep = true;
    // if there are 2 drones total the episode is over
    if (e->numDrones == 2) {
        return;
    }

    b2Body_Disable(drone->bodyID);
    drone->braking = false;
    drone->chargingBurst = false;
    drone->energyFullyDepleted = false;
    drone->shotThisStep = false;
}

void droneAddEnergy(droneEntity *drone, float energy) {
    // if a burst is charging, add the energy to the burst charge
    if (drone->chargingBurst) {
        drone->burstCharge = clamp(drone->burstCharge + energy);
    } else {
        drone->energyLeft = clamp(drone->energyLeft + energy);
    }
}

void createProjectile(env *e, droneEntity *drone, const b2Vec2 normAim) {
    ASSERT_VEC_NORMALIZED(normAim);

    const float radius = drone->weaponInfo->radius;
    float droneRadius = DRONE_RADIUS;
    if (drone->shield != NULL) {
        droneRadius = DRONE_SHIELD_RADIUS;
    }
    // spawn the projectile just outside the drone so they don't
    // immediately collide
    b2Vec2 pos = b2MulAdd(drone->pos, droneRadius + (radius * 1.5f), normAim);
    // if the projectile is inside a wall or out of the map, move the
    // projectile to be just outside the wall
    bool projectileInWall = false;
    int16_t cellIdx = entityPosToCellIdx(e, pos);
    if (cellIdx == -1) {
        projectileInWall = true;
    } else {
        const mapCell *cell = safe_array_get_at(e->cells, cellIdx);
        if (cell->ent != NULL && entityTypeIsWall(cell->ent->type)) {
            projectileInWall = true;
        }
    }
    if (projectileInWall) {
        const b2Vec2 rayEnd = b2MulAdd(drone->pos, droneRadius + (radius * 2.5f), normAim);
        const b2Vec2 translation = b2Sub(rayEnd, drone->pos);
        const b2QueryFilter filter = {.categoryBits = PROJECTILE_SHAPE, .maskBits = WALL_SHAPE};
        const b2RayResult rayRes = b2World_CastRayClosest(e->worldID, drone->pos, translation, filter);
        if (rayRes.hit) {
            const b2Vec2 invNormAim = b2MulSV(-1.0f, normAim);
            pos = b2MulAdd(rayRes.point, radius * 1.5f, invNormAim);
        }
    }

    b2BodyDef projectileBodyDef = b2DefaultBodyDef();
    projectileBodyDef.type = b2_dynamicBody;
    projectileBodyDef.isBullet = drone->weaponInfo->isPhysicsBullet;
    projectileBodyDef.linearDamping = drone->weaponInfo->damping;
    projectileBodyDef.enableSleep = drone->weaponInfo->canSleep;
    projectileBodyDef.position = pos;
    b2BodyId projectileBodyID = b2CreateBody(e->worldID, &projectileBodyDef);
    b2ShapeDef projectileShapeDef = b2DefaultShapeDef();
    projectileShapeDef.enableContactEvents = true;
    projectileShapeDef.density = drone->weaponInfo->density;
    projectileShapeDef.restitution = 1.0f;
    projectileShapeDef.filter.categoryBits = PROJECTILE_SHAPE;
    projectileShapeDef.filter.maskBits = WALL_SHAPE | FLOATING_WALL_SHAPE | PROJECTILE_SHAPE | DRONE_SHAPE | SHIELD_SHAPE;
    const b2Circle projectileCircle = {.center = b2Vec2_zero, .radius = radius};

    b2ShapeId projectileShapeID = b2CreateCircleShape(projectileBodyID, &projectileShapeDef, &projectileCircle);

    // add lateral drone velocity to projectile
    b2Vec2 forwardVel = b2MulSV(b2Dot(drone->velocity, normAim), normAim);
    b2Vec2 lateralVel = b2Sub(drone->velocity, forwardVel);
    lateralVel = b2MulSV(projectileShapeDef.density * DRONE_MOVE_AIM_COEF, lateralVel);
    b2Vec2 aim = weaponAdjustAim(&e->randState, drone->weaponInfo->type, drone->heat, normAim);
    b2Vec2 fire = b2MulAdd(lateralVel, weaponFire(&e->randState, drone->weaponInfo->type), aim);
    b2Body_ApplyLinearImpulseToCenter(projectileBodyID, fire, true);

    projectileEntity *projectile = fastCalloc(1, sizeof(projectileEntity));
    projectile->droneIdx = drone->idx;
    projectile->bodyID = projectileBodyID;
    projectile->shapeID = projectileShapeID;
    projectile->weaponInfo = drone->weaponInfo;
    projectile->pos = projectileBodyDef.position;
    projectile->lastPos = projectileBodyDef.position;
    projectile->velocity = b2Body_GetLinearVelocity(projectileBodyID);
    projectile->lastVelocity = projectile->velocity;
    projectile->speed = b2Length(projectile->velocity);
    projectile->lastSpeed = projectile->speed;
    cc_array_add(e->projectiles, projectile);

    entity *ent = fastCalloc(1, sizeof(entity));
    ent->type = PROJECTILE_ENTITY;
    ent->entity = projectile;

    projectile->ent = ent;
    b2Body_SetUserData(projectile->bodyID, ent);
    b2Shape_SetUserData(projectile->shapeID, ent);

    // create a sensor shape if needed
    if (projectile->weaponInfo->proximityDetonates) {
        projectile->sensorID = weaponSensor(projectile->bodyID, projectile->weaponInfo->type);
        b2Shape_SetUserData(projectile->sensorID, ent);
    }
}

// compute value generally from 0-1 based off of how much a projectile(s)
// or explosion(s) caused the hit drone to change velocity
float computeHitStrength(const droneEntity *hitDrone) {
    const float prevSpeed = b2Length(hitDrone->lastVelocity);
    const float curSpeed = b2Length(hitDrone->velocity);
    return fabsf(curSpeed - prevSpeed) / MAX_SPEED;
}

// simplified and copied from box2d/src/shape.c
float getShapeProjectedPerimeter(const b2ShapeId shapeID, const b2Vec2 line) {
    if (b2Shape_GetType(shapeID) == b2_circleShape) {
        const b2Circle circle = b2Shape_GetCircle(shapeID);
        return circle.radius * 2.0f;
    }

    const b2Polygon polygon = b2Shape_GetPolygon(shapeID);
    const b2Vec2 *points = polygon.vertices;
    int count = polygon.count;
    B2_ASSERT(count > 0);
    float value = b2Dot(points[0], line);
    float lower = value;
    float upper = value;
    for (int i = 1; i < count; ++i) {
        value = b2Dot(points[i], line);
        lower = b2MinFloat(lower, value);
        upper = b2MaxFloat(upper, value);
    }

    return upper - lower;
}

// explodes projectile and ensures any other projectiles that are caught
// in the explosion are also destroyed if necessary
void createProjectileExplosion(env *e, projectileEntity *projectile, const bool initalProjectile) {
    if (projectile->needsToBeDestroyed) {
        return;
    }
    projectile->needsToBeDestroyed = true;
    cc_array_add(e->explodingProjectiles, projectile);

    b2ExplosionDef explosion;
    weaponExplosion(projectile->weaponInfo->type, &explosion);
    explosion.position = projectile->pos;
    explosion.maskBits = FLOATING_WALL_SHAPE | PROJECTILE_SHAPE | DRONE_SHAPE;
    droneEntity *parentDrone = safe_array_get_at(e->drones, projectile->droneIdx);
    createExplosion(e, parentDrone, projectile, &explosion);

    if (e->client != NULL) {
        explosionInfo *explInfo = fastCalloc(1, sizeof(explosionInfo));
        explInfo->def = explosion;
        explInfo->renderSteps = UINT16_MAX;
        cc_array_add(e->explosions, explInfo);
    }
    if (!initalProjectile) {
        return;
    }

    // if we're not destroying the projectiles now, we need to remove the initial projectile
    // from the list of exploding projectiles so it's not destroyed twice
    const enum cc_stat res = cc_array_remove_fast(e->explodingProjectiles, projectile, NULL);
    MAYBE_UNUSED(res);
    ASSERT(res == CC_OK);
}

typedef struct explosionCtx {
    env *e;
    const bool isBurst;
    droneEntity *parentDrone;
    const projectileEntity *projectile;
    const b2ExplosionDef *def;
} explosionCtx;

// b2World_Explode doesn't support filtering on shapes of the same category,
// so we have to do it manually
// mostly copied from box2d/src/world.c
bool explodeCallback(b2ShapeId shapeID, void *context) {
    if (!b2Shape_IsValid(shapeID)) {
        return true;
    }

    const explosionCtx *ctx = context;
    const entity *entity = b2Shape_GetUserData(shapeID);
    projectileEntity *projectile = NULL;
    droneEntity *drone = NULL;
    wallEntity *wall = NULL;
    bool isStaticWall = false;
    bool isFloatingWall = false;
    b2Transform transform;

    switch (entity->type) {
    case PROJECTILE_ENTITY:
        // don't explode the parent projectile
        projectile = entity->entity;
        if (ctx->projectile != NULL && (ctx->projectile == projectile || projectile->needsToBeDestroyed)) {
            return true;
        }
        transform.p = projectile->pos;
        transform.q = b2Rot_identity;
        break;
    case DRONE_ENTITY:
        drone = entity->entity;
        // the explosion shouldn't affect the parent drone if this is a burst
        if (drone->idx == ctx->parentDrone->idx) {
            if (ctx->isBurst) {
                return true;
            }

            drone->stepInfo.ownShotTaken = true;
            ctx->e->stats[drone->idx].ownShotsTaken[ctx->projectile->weaponInfo->type]++;
            DEBUG_LOGF("drone %d hit itself with explosion from weapon %d", drone->idx, ctx->projectile->weaponInfo->type);
        }
        ctx->parentDrone->stepInfo.explosionHit[drone->idx] = true;
        if (ctx->isBurst) {
            DEBUG_LOGF("drone %d hit drone %d with burst", ctx->parentDrone->idx, drone->idx);
            ctx->e->stats[ctx->parentDrone->idx].burstsHit++;
            DEBUG_LOGF("drone %d hit by burst from drone %d", drone->idx, ctx->parentDrone->idx);
        } else {
            DEBUG_LOGF("drone %d hit drone %d with explosion from weapon %d", ctx->parentDrone->idx, drone->idx, ctx->projectile->weaponInfo->type);
            ctx->e->stats[ctx->parentDrone->idx].shotsHit[ctx->projectile->weaponInfo->type]++;
            DEBUG_LOGF("drone %d hit by explosion from weapon %d from drone %d", drone->idx, ctx->projectile->weaponInfo->type, ctx->parentDrone->idx);
        }
        drone->stepInfo.explosionTaken[ctx->parentDrone->idx] = true;
        transform.p = drone->pos;
        transform.q = b2Rot_identity;
        break;
    case STANDARD_WALL_ENTITY:
    case BOUNCY_WALL_ENTITY:
    case DEATH_WALL_ENTITY:
        wall = entity->entity;
        isStaticWall = !wall->isFloating;
        isFloatingWall = wall->isFloating;
        // normal explosions don't affect static walls
        if (!ctx->isBurst && isStaticWall) {
            return true;
        }
        transform.p = wall->pos;
        transform.q = wall->rot;
        break;
    default:
        ERRORF("invalid entity type %d to explode", entity->type);
    }

    // find the closest point from the entity to the explosion center
    const b2BodyId bodyID = b2Shape_GetBody(shapeID);
    ASSERT(b2Body_IsValid(bodyID));

    bool isCircle = false;
    b2DistanceInput input;
    input.proxyA = makeDistanceProxy(entity, &isCircle);
    input.proxyB = b2MakeProxy(&ctx->def->position, 1, 0.0f);
    input.transformA = transform;
    input.transformB = b2Transform_identity;
    input.useRadii = isCircle;

    b2SimplexCache cache = {0};
    const b2DistanceOutput output = b2ShapeDistance(&cache, &input, NULL, 0);
    // don't consider falloff for static walls so burst pushback isn't as
    // surprising to players
    if (output.distance > ctx->def->radius + ctx->def->falloff || (isStaticWall && output.distance > ctx->def->radius)) {
        return true;
    }

    // don't explode the entity if it's behind a static or floating wall,
    // but always consider floating walls for implosions
    const bool isImplosion = ctx->def->impulsePerLength < 0.0f;
    b2QueryFilter filter = {.categoryBits = PROJECTILE_SHAPE, .maskBits = WALL_SHAPE};
    if (!isImplosion) {
        filter.maskBits |= FLOATING_WALL_SHAPE;
    }
    if (!isStaticWall && posBehindWall(ctx->e, ctx->def->position, output.pointA, entity, filter, NULL)) {
        return true;
    }

    const b2Vec2 closestPoint = output.pointA;
    b2Vec2 direction;
    if (isStaticWall) {
        direction = b2Normalize(b2Sub(ctx->def->position, closestPoint));
    } else {
        direction = b2Normalize(b2Sub(closestPoint, ctx->def->position));
    }
    // if the direction is zero, the magnitude cannot be calculated
    // correctly so set the direction randomly
    if (b2VecEqual(direction, b2Vec2_zero)) {
        direction.x = randFloat(&ctx->e->randState, -1.0f, 1.0f);
        direction.y = randFloat(&ctx->e->randState, -1.0f, 1.0f);
        direction = b2Normalize(direction);
    }

    b2Vec2 localLine = b2Vec2_zero;
    if (entityTypeIsWall(entity->type)) {
        // the localLine isn't used in perimeter calculations for circles
        localLine = b2InvRotateVector(transform.q, b2LeftPerp(direction));
    }
    float perimeter = getShapeProjectedPerimeter(shapeID, localLine);
    float scale = 1.0f;
    // ignore falloff for projectiles to avoid slowing them down to a crawl
    if (output.distance > ctx->def->radius && entity->type != PROJECTILE_ENTITY) {
        scale = clamp((ctx->def->radius + ctx->def->falloff - output.distance) / ctx->def->falloff);
    }

    // the parent drone or projecile's velocity affects the direction
    // and magnitude of the explosion
    b2Vec2 parentVelocity;
    float parentSpeed;
    if (ctx->projectile != NULL) {
        // use the projectile's last velocity and speed if it is in
        // contact with another body, as the current velocity will be
        // the velocity after the projectile rebounds which is not
        // what we want
        if (ctx->projectile->contacts != 0) {
            parentVelocity = ctx->projectile->lastVelocity;
            parentSpeed = ctx->projectile->lastSpeed;
        } else {
            parentVelocity = ctx->projectile->velocity;
            parentSpeed = ctx->projectile->speed;
        }
        if (isImplosion) {
            parentSpeed *= -1.0f;
        }
    } else {
        parentVelocity = ctx->parentDrone->lastVelocity;
        parentSpeed = b2Length(parentVelocity);
    }
    const b2Vec2 parentDirection = b2Normalize(parentVelocity);

    // scale the parent speed by how close the movement direction of
    // the parent is to where the entity is to the parent, except if
    // we're bursting off of a wall to make it more predictable and
    // to prevent taking a log of a negative number
    if (!isStaticWall) {
        parentSpeed *= b2Dot(direction, parentDirection);
    }
    // don't change the direction of the explosion if this is a burst
    // hitting a projectile or static wall to make it more predictable
    const b2Vec2 baseImpulse = b2MulSV(fabsf(ctx->def->impulsePerLength), direction);
    direction = b2Normalize(b2Add(baseImpulse, parentVelocity));

    float shieldReduction = 1.0f;
    if (drone != NULL && drone->shield != NULL) {
        shieldReduction = DRONE_SHIELD_EXPLOSION_REDUCTION;
    }

    float magnitude = (ctx->def->impulsePerLength + parentSpeed) * perimeter * scale * shieldReduction;
    if (isStaticWall) {
        // reduce the magnitude when pushing a drone away from a wall
        magnitude = log2f(magnitude) * 7.5f;
    }
    const b2Vec2 impulse = b2MulSV(magnitude, direction);

    if (isStaticWall) {
        b2Body_ApplyLinearImpulseToCenter(ctx->parentDrone->bodyID, impulse, true);
    } else {
        b2Body_ApplyLinearImpulseToCenter(bodyID, impulse, true);
        switch (entity->type) {
        case STANDARD_WALL_ENTITY:
        case BOUNCY_WALL_ENTITY:
        case DEATH_WALL_ENTITY:
            wall->velocity = b2Body_GetLinearVelocity(wall->bodyID);
            if (isFloatingWall) {
                // floating walls are the only bodies that can rotate
                b2Body_ApplyAngularImpulse(bodyID, magnitude, true);
            }
            break;
        case PROJECTILE_ENTITY:
            // mine launcher projectiles explode when caught in another
            // explosion, explode this mine only once
            if (projectile->weaponInfo->type == MINE_LAUNCHER_WEAPON && ctx->def->impulsePerLength > 0.0f) {
                createProjectileExplosion(ctx->e, projectile, false);
                break;
            }

            // ensure projectiles don't slow down
            b2Vec2 newVel = b2Body_GetLinearVelocity(projectile->bodyID);
            float newSpeed = b2Length(newVel);
            if (newSpeed < projectile->lastSpeed) {
                newSpeed = projectile->lastSpeed;
                newVel = b2MulSV(newSpeed, b2Normalize(newVel));
                b2Body_SetLinearVelocity(projectile->bodyID, newVel);
            }

            projectile->velocity = newVel;
            projectile->lastSpeed = newSpeed;
            projectile->speed = newSpeed;
            break;
        case DRONE_ENTITY:
            drone->lastVelocity = drone->velocity;
            drone->velocity = b2Body_GetLinearVelocity(drone->bodyID);

            // add energy to the drone that fired the projectile that is
            // currently exploding if it hit another drone
            if (!ctx->isBurst && drone->team != ctx->parentDrone->team) {
                const float energyRefill = computeHitStrength(drone) * EXPLOSION_ENERGY_REFILL_COEF;
                droneAddEnergy(ctx->parentDrone, energyRefill);
            }
            break;
        default:
            ERRORF("unknown entity type for burst impulse %d", entity->type);
        }
    }

    return true;
}

void createExplosion(env *e, droneEntity *drone, const projectileEntity *projectile, const b2ExplosionDef *def) {
    const float fullRadius = def->radius + def->falloff;
    b2AABB aabb = {
        .lowerBound.x = def->position.x - fullRadius,
        .lowerBound.y = def->position.y - fullRadius,
        .upperBound.x = def->position.x + fullRadius,
        .upperBound.y = def->position.y + fullRadius,
    };

    b2QueryFilter filter = b2DefaultQueryFilter();
    filter.categoryBits = PROJECTILE_SHAPE;
    filter.maskBits = def->maskBits;

    explosionCtx ctx = {
        .e = e,
        .isBurst = projectile == NULL,
        .parentDrone = drone,
        .projectile = projectile,
        .def = def,
    };
    b2World_OverlapAABB(e->worldID, aabb, filter, explodeCallback, &ctx);
}

void destroyProjectile(env *e, projectileEntity *projectile, const bool processExplosions, const bool full) {
    // explode projectile if necessary
    if (processExplosions && projectile->weaponInfo->explosive) {
        createProjectileExplosion(e, projectile, true);
    }

    fastFree(projectile->ent);

    b2DestroyBody(projectile->bodyID);

    if (full) {
        enum cc_stat res = cc_array_remove_fast(e->projectiles, projectile, NULL);
        MAYBE_UNUSED(res);
        ASSERT(res == CC_OK);
    }

    e->stats[projectile->droneIdx].shotDistances[projectile->droneIdx] += projectile->distance;

    fastFree(projectile);
}

// destroy projectiles that were caught in an explosion; projectiles
// can't be destroyed in explodeCallback because box2d assumes all shapes
// and bodies are valid for the lifetime of an AABB query
static inline void destroyExplodedProjectiles(env *e) {
    if (cc_array_size(e->explodingProjectiles) == 0) {
        return;
    }

    CC_ArrayIter iter;
    cc_array_iter_init(&iter, e->explodingProjectiles);
    projectileEntity *projectile;
    while (cc_array_iter_next(&iter, (void **)&projectile) != CC_ITER_END) {
        destroyProjectile(e, projectile, false, false);
        const enum cc_stat res = cc_array_remove_fast(e->projectiles, projectile, NULL);
        MAYBE_UNUSED(res);
        ASSERT(res == CC_OK);
    }
    cc_array_remove_all(e->explodingProjectiles);
}

void createSuddenDeathWalls(env *e, const b2Vec2 startPos, const b2Vec2 size) {
    int16_t endIdx;
    uint8_t indexIncrement;
    if (size.y == WALL_THICKNESS) {
        // horizontal walls
        const b2Vec2 endPos = (b2Vec2){.x = startPos.x + size.x, .y = startPos.y};
        endIdx = entityPosToCellIdx(e, endPos);
        if (endIdx == -1) {
            ERRORF("invalid position for sudden death wall: (%f, %f)", endPos.x, endPos.y);
        }
        indexIncrement = 1;
    } else {
        // vertical walls
        const b2Vec2 endPos = (b2Vec2){.x = startPos.x, .y = startPos.y + size.y};
        endIdx = entityPosToCellIdx(e, endPos);
        if (endIdx == -1) {
            ERRORF("invalid position for sudden death wall: (%f, %f)", endPos.x, endPos.y);
        }
        indexIncrement = e->map->columns;
    }
    const int16_t startIdx = entityPosToCellIdx(e, startPos);
    if (startIdx == -1) {
        ERRORF("invalid position for sudden death wall: (%f, %f)", startPos.x, startPos.y);
    }
    for (uint16_t i = startIdx; i <= endIdx; i += indexIncrement) {
        mapCell *cell = safe_array_get_at(e->cells, i);
        if (cell->ent != NULL) {
            if (cell->ent->type == WEAPON_PICKUP_ENTITY) {
                weaponPickupEntity *pickup = cell->ent->entity;
                disableWeaponPickup(e, pickup);
            } else {
                continue;
            }
        }
        entity *ent = createWall(e, cell->pos, WALL_THICKNESS, WALL_THICKNESS, i, DEATH_WALL_ENTITY, false);
        cell->ent = ent;
    }
}

void handleSuddenDeath(env *e) {
    ASSERT(e->suddenDeathSteps == 0);

    // create new walls that will close in on the arena
    e->suddenDeathWallCounter++;
    e->suddenDeathWallsPlaced = true;

    const float leftX = (e->suddenDeathWallCounter - 1) * WALL_THICKNESS;
    const float yOffset = (WALL_THICKNESS * (e->suddenDeathWallCounter - 1)) + (WALL_THICKNESS / 2);
    const float xWidth = WALL_THICKNESS * (e->map->columns - (e->suddenDeathWallCounter * 2) - 1);

    // top walls
    createSuddenDeathWalls(
        e,
        (b2Vec2){
            .x = e->map->bounds.min.x + leftX,
            .y = e->map->bounds.min.y + yOffset,
        },
        (b2Vec2){
            .x = xWidth,
            .y = WALL_THICKNESS,
        });
    // bottom walls
    createSuddenDeathWalls(
        e,
        (b2Vec2){
            .x = e->map->bounds.min.x + leftX,
            .y = e->map->bounds.max.y - yOffset,
        },
        (b2Vec2){
            .x = xWidth,
            .y = WALL_THICKNESS,
        });
    // left walls
    createSuddenDeathWalls(
        e,
        (b2Vec2){
            .x = e->map->bounds.min.x + leftX,
            .y = e->map->bounds.min.y + (e->suddenDeathWallCounter * WALL_THICKNESS),
        },
        (b2Vec2){
            .x = WALL_THICKNESS,
            .y = WALL_THICKNESS * (e->map->rows - (e->suddenDeathWallCounter * 2) - 2),
        });
    // right walls
    createSuddenDeathWalls(
        e,
        (b2Vec2){
            .x = e->map->bounds.min.x + ((e->map->columns - e->suddenDeathWallCounter - 2) * WALL_THICKNESS),
            .y = e->map->bounds.min.y + (e->suddenDeathWallCounter * WALL_THICKNESS),
        },
        (b2Vec2){
            .x = WALL_THICKNESS,
            .y = WALL_THICKNESS * (e->map->rows - (e->suddenDeathWallCounter * 2) - 2),
        });

    // mark drones as dead if they touch a newly placed wall
    uint8_t deadDrones = 0;
    for (uint8_t i = 0; i < e->numDrones; i++) {
        droneEntity *drone = safe_array_get_at(e->drones, i);
        const b2QueryFilter filter = {
            .categoryBits = DRONE_SHAPE,
            .maskBits = WALL_SHAPE,
        };
        if (isOverlappingAABB(e, drone->pos, DRONE_RADIUS, filter)) {
            killDrone(e, drone);
            deadDrones++;
        }
    }
    // if all but one or all drones are dead, the episode is over
    if (deadDrones >= e->numDrones - 1) {
        return;
    }

    // make floating walls static bodies if they are now overlapping with
    // a newly placed wall, but destroy them if they are fully inside a wall
    CC_ArrayIter floatingWallIter;
    cc_array_iter_init(&floatingWallIter, e->floatingWalls);
    wallEntity *wall;
    while (cc_array_iter_next(&floatingWallIter, (void **)&wall) != CC_ITER_END) {
        const mapCell *cell = safe_array_get_at(e->cells, wall->mapCellIdx);
        if (cell->ent != NULL && entityTypeIsWall(cell->ent->type)) {
            // floating wall is overlapping with a wall, destroy it
            const enum cc_stat res = cc_array_iter_remove_fast(&floatingWallIter, NULL);
            MAYBE_UNUSED(res);
            ASSERT(res == CC_OK);

            const b2Vec2 wallPos = wall->pos;
            MAYBE_UNUSED(wallPos);
            destroyWall(e, wall, false);
            DEBUG_LOGF("destroyed floating wall at %f, %f", wallPos.x, wallPos.y);
            continue;
        }
    }

    // detroy all projectiles that are now overlapping with a newly placed wall
    CC_ArrayIter projectileIter;
    cc_array_iter_init(&projectileIter, e->projectiles);
    projectileEntity *projectile;
    while (cc_array_iter_next(&projectileIter, (void **)&projectile) != CC_ITER_END) {
        const mapCell *cell = safe_array_get_at(e->cells, projectile->mapCellIdx);
        if (cell->ent != NULL && entityTypeIsWall(cell->ent->type)) {
            cc_array_iter_remove_fast(&projectileIter, NULL);
            destroyProjectile(e, projectile, false, false);
        }
    }
}

void droneMove(droneEntity *drone, b2Vec2 direction) {
    ASSERT_VEC_BOUNDED(direction);

    // if energy is fully depleted halve movement until energy starts
    // to refill again
    if (drone->energyFullyDepleted && drone->energyRefillWait != 0.0f) {
        direction = b2MulSV(0.5f, direction);
        drone->lastMove = direction;
    }
    const b2Vec2 force = b2MulSV(DRONE_MOVE_MAGNITUDE, direction);
    b2Body_ApplyForceToCenter(drone->bodyID, force, true);
}

void droneChangeWeapon(const env *e, droneEntity *drone, const enum weaponType newWeapon) {
    // top up ammo but change nothing else if the weapon is the same
    if (drone->weaponInfo->type != newWeapon) {
        drone->weaponCooldown = 0.0f;
        drone->weaponCharge = 0.0f;
        drone->heat = 0;
    }
    drone->weaponInfo = weaponInfos[newWeapon];
    drone->ammo = weaponAmmo(e->defaultWeapon->type, drone->weaponInfo->type);
}

void droneShoot(env *e, droneEntity *drone, const b2Vec2 aim, const bool chargingWeapon) {
    ASSERT(drone->ammo != 0);

    drone->shotThisStep = true;
    // TODO: rework heat to only increase when projectiles are fired,
    // and only cool down after the next shot was skipped
    drone->heat++;
    if (drone->weaponCooldown != 0.0f) {
        return;
    }
    const bool weaponNeedsCharge = drone->weaponInfo->charge != 0.0f;
    if (weaponNeedsCharge) {
        if (chargingWeapon) {
            drone->chargingWeapon = true;
            drone->weaponCharge = fminf(drone->weaponCharge + e->deltaTime, drone->weaponInfo->charge);
        } else if (drone->weaponCharge < drone->weaponInfo->charge) {
            drone->chargingWeapon = false;
            drone->weaponCharge = fmaxf(drone->weaponCharge - e->deltaTime, 0.0f);
        }
    }
    // if the weapon needs to be charged, only fire the weapon if it's
    // fully charged and the agent released the trigger
    if (weaponNeedsCharge && (chargingWeapon || drone->weaponCharge < drone->weaponInfo->charge)) {
        return;
    }

    if (drone->ammo != INFINITE) {
        drone->ammo--;
    }
    drone->weaponCooldown = drone->weaponInfo->coolDown;
    drone->chargingWeapon = false;
    drone->weaponCharge = 0.0f;

    b2Vec2 normAim = drone->lastAim;
    if (!b2VecEqual(aim, b2Vec2_zero)) {
        normAim = b2Normalize(aim);
    }
    ASSERT_VEC_NORMALIZED(normAim);
    b2Vec2 recoil = b2MulSV(-drone->weaponInfo->recoilMagnitude, normAim);
    b2Body_ApplyLinearImpulseToCenter(drone->bodyID, recoil, true);

    for (int i = 0; i < drone->weaponInfo->numProjectiles; i++) {
        createProjectile(e, drone, normAim);

        e->stats[drone->idx].shotsFired[drone->weaponInfo->type]++;
    }
    drone->stepInfo.firedShot = true;

    if (drone->ammo == 0) {
        droneChangeWeapon(e, drone, e->defaultWeapon->type);
        drone->weaponCooldown = drone->weaponInfo->coolDown;
    }
}

void droneBrake(env *e, droneEntity *drone, const bool brake) {
    // if the drone isn't braking or energy is fully depleted, return
    // unless the drone was braking during the last step
    if (!brake || drone->energyFullyDepleted) {
        if (drone->braking) {
            drone->braking = false;
            b2Body_SetLinearDamping(drone->bodyID, DRONE_LINEAR_DAMPING);
            if (drone->energyRefillWait == 0.0f && !drone->chargingBurst) {
                drone->energyRefillWait = DRONE_ENERGY_REFILL_WAIT;
            }
        }
        return;
    }
    ASSERT(!drone->energyFullyDepleted);

    // apply additional brake damping and decrease energy
    if (brake) {
        if (!drone->braking) {
            drone->braking = true;
            b2Body_SetLinearDamping(drone->bodyID, DRONE_LINEAR_DAMPING * DRONE_BRAKE_DAMPING_COEF);
        }
        drone->energyLeft = fmaxf(drone->energyLeft - (DRONE_BRAKE_DRAIN_RATE * e->deltaTime), 0.0f);
        e->stats[drone->idx].brakeTime += e->deltaTime;
    }

    // if energy is empty but burst is being charged, let burst functions
    // handle energy refill
    if (drone->energyLeft == 0.0f && !drone->chargingBurst) {
        drone->energyFullyDepleted = true;
        drone->energyFullyDepletedThisStep = true;
        drone->energyRefillWait = DRONE_ENERGY_REFILL_EMPTY_WAIT;
        e->stats[drone->idx].energyEmptied++;
    }

    if (e->client != NULL) {
        brakeTrailPoint *trailPoint = fastCalloc(1, sizeof(brakeTrailPoint));
        trailPoint->pos = drone->pos;
        trailPoint->lifetime = UINT16_MAX;
        cc_array_add(e->brakeTrailPoints, trailPoint);
    }
}

void droneChargeBurst(env *e, droneEntity *drone) {
    if (drone->energyFullyDepleted || drone->burstCooldown != 0.0f || (!drone->chargingBurst && drone->energyLeft < DRONE_BURST_BASE_COST)) {
        return;
    }

    // take energy and put it into burst charge
    if (drone->chargingBurst) {
        drone->burstCharge = fminf(drone->burstCharge + (DRONE_BURST_CHARGE_RATE * e->deltaTime), DRONE_ENERGY_MAX);
        drone->energyLeft = fmaxf(drone->energyLeft - (DRONE_BURST_CHARGE_RATE * e->deltaTime), 0.0f);
    } else {
        drone->burstCharge = fminf(drone->burstCharge + DRONE_BURST_BASE_COST, DRONE_ENERGY_MAX);
        drone->energyLeft = fmaxf(drone->energyLeft - DRONE_BURST_BASE_COST, 0.0f);
        drone->chargingBurst = true;
    }

    if (drone->energyLeft == 0.0f) {
        drone->energyFullyDepleted = true;
        e->stats[drone->idx].energyEmptied++;
    }
}

void droneBurst(env *e, droneEntity *drone) {
    if (!drone->chargingBurst) {
        return;
    }

    const float radius = (DRONE_BURST_RADIUS_BASE * drone->burstCharge) + DRONE_BURST_RADIUS_MIN;
    b2ExplosionDef explosion = {
        .position = drone->pos,
        .radius = radius,
        .impulsePerLength = (DRONE_BURST_IMPACT_BASE * drone->burstCharge) + DRONE_BURST_IMPACT_MIN,
        .falloff = radius / 2.0f,
        .maskBits = WALL_SHAPE | FLOATING_WALL_SHAPE | PROJECTILE_SHAPE | DRONE_SHAPE,
    };
    createExplosion(e, drone, NULL, &explosion);
    destroyExplodedProjectiles(e);

    drone->chargingBurst = false;
    drone->burstCharge = 0.0f;
    drone->burstCooldown = DRONE_BURST_COOLDOWN;
    if (drone->energyLeft == 0.0f) {
        drone->energyFullyDepletedThisStep = true;
        drone->energyRefillWait = DRONE_ENERGY_REFILL_EMPTY_WAIT;
    } else {
        drone->energyRefillWait = DRONE_ENERGY_REFILL_WAIT;
    }
    e->stats[drone->idx].totalBursts++;

    if (e->client != NULL) {
        explosionInfo *explInfo = fastCalloc(1, sizeof(explosionInfo));
        explInfo->def = explosion;
        explInfo->renderSteps = UINT16_MAX;
        cc_array_add(e->explosions, explInfo);
    }
}

void droneDiscardWeapon(env *e, droneEntity *drone) {
    if (drone->weaponInfo->type == e->defaultWeapon->type || (drone->energyFullyDepleted && !drone->chargingBurst)) {
        return;
    }

    droneChangeWeapon(e, drone, e->defaultWeapon->type);
    droneAddEnergy(drone, -WEAPON_DISCARD_COST);
    if (drone->chargingBurst) {
        return;
    }

    if (drone->energyLeft == 0.0f) {
        drone->energyFullyDepleted = true;
        drone->energyFullyDepletedThisStep = true;
        drone->energyRefillWait = DRONE_ENERGY_REFILL_EMPTY_WAIT;
        e->stats[drone->idx].energyEmptied++;
    } else {
        drone->energyRefillWait = DRONE_ENERGY_REFILL_WAIT;
    }
}

typedef struct castCircleCtx {
    bool hit;
    b2ShapeId shapeID;
} castCircleCtx;

float castCircleCallback(b2ShapeId shapeId, b2Vec2 point, b2Vec2 normal, float fraction, void *context) {
    // these parameters are required by the callback signature
    MAYBE_UNUSED(point);
    MAYBE_UNUSED(normal);
    if (!b2Shape_IsValid(shapeId)) {
        // skip this shape if it isn't valid
        return -1.0f;
    }

    castCircleCtx *ctx = context;
    ctx->hit = true;
    ctx->shapeID = shapeId;

    return fraction;
}

void droneStep(env *e, droneEntity *drone) {
    // manage weapon charge and heat
    if (drone->weaponCooldown != 0.0f) {
        drone->weaponCooldown = fmaxf(drone->weaponCooldown - e->deltaTime, 0.0f);
    }
    if (!drone->shotThisStep) {
        drone->weaponCharge = fmaxf(drone->weaponCharge - e->deltaTime, 0);
        drone->heat = fmaxf(drone->heat - 1, 0);
    } else {
        drone->shotThisStep = false;
    }
    ASSERT(!drone->shotThisStep);

    // manage drone energy
    if (drone->burstCooldown != 0.0f) {
        drone->burstCooldown = fmaxf(drone->burstCooldown - e->deltaTime, 0.0f);
    }
    if (drone->energyFullyDepletedThisStep) {
        drone->energyFullyDepletedThisStep = false;
    } else if (drone->energyRefillWait != 0.0f) {
        drone->energyRefillWait = fmaxf(drone->energyRefillWait - e->deltaTime, 0.0f);
    } else if (drone->energyLeft != DRONE_ENERGY_MAX && !drone->chargingBurst) {
        // don't start recharging energy until the burst charge is used
        drone->energyLeft = fminf(drone->energyLeft + (DRONE_ENERGY_REFILL_RATE * e->deltaTime), DRONE_ENERGY_MAX);
    }
    if (drone->energyLeft == DRONE_ENERGY_MAX) {
        drone->energyFullyDepleted = false;
    }

    const float distance = b2Distance(drone->lastPos, drone->pos);
    e->stats[drone->idx].distanceTraveled += distance;

    shieldEntity *shield = drone->shield;
    if (shield != NULL) {
        shield->duration = fmaxf(shield->duration - e->deltaTime, 0.0f);
        if (shield->duration == 0.0f || shield->health == 0.0f) {
            destroyDroneShield(shield);
        }
    }
}

void projectilesStep(env *e) {
    CC_ArrayIter iter;
    cc_array_iter_init(&iter, e->projectiles);
    projectileEntity *projectile;
    while (cc_array_iter_next(&iter, (void **)&projectile) != CC_ITER_END) {
        if (projectile->needsToBeDestroyed) {
            continue;
        }
        const float maxDistance = projectile->weaponInfo->maxDistance;
        const float distance = b2Distance(projectile->pos, projectile->lastPos);
        projectile->distance += distance;

        // if a drone is in a set mine's sensor range but behind a wall,
        // we need to check until the drone leaves the sensor range if
        // it's not behind the wall anymore as we normally only check if
        // we need to explode the mine when a drone touches the sensor
        if (projectile->numDronesBehindWalls != 0) {
            bool destroyed = false;
            for (uint8_t i = 0; i < projectile->numDronesBehindWalls; i++) {
                const uint8_t droneIdx = projectile->dronesBehindWalls[i];
                const droneEntity *drone = safe_array_get_at(e->drones, droneIdx);
                const b2DistanceOutput output = closestPoint(projectile->ent, drone->ent);
                const b2QueryFilter filter = {.categoryBits = PROJECTILE_SHAPE, .maskBits = WALL_SHAPE | FLOATING_WALL_SHAPE};
                if (posBehindWall(e, projectile->pos, output.pointB, NULL, filter, NULL)) {
                    continue;
                }

                // we have to destroy the projectile using the iterator so
                // we can continue to iterate correctly
                destroyProjectile(e, projectile, true, false);
                enum cc_stat res = cc_array_iter_remove_fast(&iter, NULL);
                MAYBE_UNUSED(res);
                ASSERT(res == CC_OK);
                destroyed = true;
                break;
            }
            if (destroyed) {
                continue;
            }
        }

        if (maxDistance == INFINITE) {
            continue;
        }

        if (projectile->distance >= maxDistance) {
            // we have to destroy the projectile using the iterator so
            // we can continue to iterate correctly
            destroyProjectile(e, projectile, true, false);
            enum cc_stat res = cc_array_iter_remove_fast(&iter, NULL);
            MAYBE_UNUSED(res);
            ASSERT(res == CC_OK);
            continue;
        }
    }

    destroyExplodedProjectiles(e);
}

void weaponPickupsStep(env *e) {
    CC_ArrayIter iter;
    cc_array_iter_init(&iter, e->pickups);
    weaponPickupEntity *pickup;

    // respawn weapon pickups at a random location as a random weapon type
    // once the respawn wait has elapsed
    while (cc_array_iter_next(&iter, (void **)&pickup) != CC_ITER_END) {
        if (pickup->respawnWait == 0.0f) {
            continue;
        }
        pickup->respawnWait = fmaxf(pickup->respawnWait - e->deltaTime, 0.0f);
        if (pickup->respawnWait != 0.0f) {
            continue;
        }

        b2Vec2 pos;
        if (!findOpenPos(e, WEAPON_PICKUP_SHAPE, &pos, -1)) {
            const enum cc_stat res = cc_array_iter_remove_fast(&iter, NULL);
            MAYBE_UNUSED(res);
            ASSERT(res == CC_OK);
            DEBUG_LOG("destroying weapon pickup");
            destroyWeaponPickup(e, pickup);
            continue;
        }
        pickup->pos = pos;
        pickup->weapon = randWeaponPickupType(e);

        const int16_t cellIdx = entityPosToCellIdx(e, pos);
        if (cellIdx == -1) {
            ERRORF("invalid position for weapon pickup spawn: (%f, %f)", pos.x, pos.y);
        }
        DEBUG_LOGF("respawned weapon pickup at cell %d (%f, %f)", cellIdx, pos.x, pos.y);
        pickup->mapCellIdx = cellIdx;
        createWeaponPickupBodyShape(e, pickup);

        mapCell *cell = safe_array_get_at(e->cells, cellIdx);
        cell->ent = pickup->ent;
    }
}

// only update positions and velocities of dynamic bodies if they moved
// this step
void handleBodyMoveEvents(env *e) {
    b2BodyEvents events = b2World_GetBodyEvents(e->worldID);
    for (int i = 0; i < events.moveCount; i++) {
        const b2BodyMoveEvent *event = events.moveEvents + i;
        if (!b2Body_IsValid(event->bodyId)) {
            continue;
        }
        ASSERT(b2IsValidVec2(event->transform.p));
        const b2Vec2 newPos = event->transform.p;
        entity *ent = event->userData;
        if (ent == NULL) {
            continue;
        }

        wallEntity *wall;
        projectileEntity *proj;
        droneEntity *drone;
        shieldEntity *shield;

        // if the new position is out of bounds, destroy the entity unless
        // a drone is out of bounds, then just kill it
        switch (ent->type) {
        case STANDARD_WALL_ENTITY:
        case BOUNCY_WALL_ENTITY:
        case DEATH_WALL_ENTITY:
            wall = ent->entity;
            wall->mapCellIdx = entityPosToCellIdx(e, newPos);
            if (wall->mapCellIdx == -1) {
                DEBUG_LOGF("invalid position for floating wall: (%f, %f) destroying", newPos.x, newPos.y);
                cc_array_remove_fast(e->floatingWalls, wall, NULL);
                destroyWall(e, wall, false);
                continue;
            }
            wall->pos = newPos;
            wall->rot = event->transform.q;
            wall->velocity = b2Body_GetLinearVelocity(wall->bodyID);
            break;
        case PROJECTILE_ENTITY:
            proj = ent->entity;
            proj->mapCellIdx = entityPosToCellIdx(e, newPos);
            if (proj->mapCellIdx == -1) {
                DEBUG_LOGF("invalid position for projectile: (%f, %f) destroying", newPos.x, newPos.y);
                destroyProjectile(e, proj, false, true);
                continue;
            }
            proj->lastPos = proj->pos;
            proj->pos = newPos;
            proj->lastVelocity = proj->velocity;
            proj->velocity = b2Body_GetLinearVelocity(proj->bodyID);
            // if the projectile doesn't have damping its speed will
            // only change when colliding with a dynamic body or getting
            // hit by an explosion, and if it's currently colliding with
            // something we don't care about the current speed
            if (proj->weaponInfo->damping != 0.0f && proj->contacts == 0) {
                proj->lastSpeed = proj->speed;
                proj->speed = b2Length(proj->velocity);
            }

            if (e->client != NULL) {
                updateTrailPoints(e, &proj->trailPoints, MAX_PROJECTLE_TRAIL_POINTS, newPos);
            }
            break;
        case DRONE_ENTITY:
            drone = ent->entity;
            drone->mapCellIdx = entityPosToCellIdx(e, newPos);
            if (drone->mapCellIdx == -1) {
                DEBUG_LOGF("invalid position for drone: (%f, %f) killing it", newPos.x, newPos.y);
                killDrone(e, drone);
                continue;
            }
            drone->lastPos = drone->pos;
            drone->pos = newPos;
            drone->lastVelocity = drone->velocity;
            drone->velocity = b2Body_GetLinearVelocity(drone->bodyID);

            if (e->client != NULL) {
                updateTrailPoints(e, &drone->trailPoints, MAX_DRONE_TRAIL_POINTS, newPos);
            }
            break;
        case SHIELD_ENTITY:
            shield = ent->entity;
            shield->pos = newPos;
            break;
        default:
            ERRORF("unknown entity type for move event %d", ent->type);
        }
    }
}

// destroy the projectile if it has traveled enough or has bounced enough
// times, and update drone stats if a drone was hit
uint8_t handleProjectileBeginContact(env *e, const entity *proj, const entity *ent, const b2Manifold *manifold, const bool projIsShapeA) {
    projectileEntity *projectile = proj->entity;
    projectile->contacts++;

    // e (shape B in the collision) will be NULL if it's another
    // projectile that was just destroyed
    if (ent == NULL || ent->type == PROJECTILE_ENTITY) {
        // explode mines when hit by a projectile
        if (projectile->weaponInfo->type == MINE_LAUNCHER_WEAPON) {
            uint8_t numDestroyed = 1;
            if (ent != NULL) {
                const projectileEntity *projectile2 = ent->entity;
                // if both entities are mines both will be destroyed
                if (projectile2->weaponInfo->type == MINE_LAUNCHER_WEAPON) {
                    numDestroyed = 2;
                }
            }
            destroyProjectile(e, projectile, true, true);
            destroyExplodedProjectiles(e);
            return numDestroyed;
        }

        // always allow all other projectiles to bounce off each other
        return false;
    } else if (ent->type == BOUNCY_WALL_ENTITY) {
        // always allow projectiles to bounce off bouncy walls
        return false;
    } else if (ent->type == SHIELD_ENTITY) {
        // always allow projectiles to bounce off shields, and update shield health
        // TODO: decrease shield health
        return false;
    }

    projectile->bounces++;
    if (ent->type == DRONE_ENTITY) {
        droneEntity *hitDrone = ent->entity;
        if (projectile->droneIdx != hitDrone->idx) {
            droneEntity *shooterDrone = safe_array_get_at(e->drones, projectile->droneIdx);

            if (shooterDrone->team != hitDrone->team) {
                droneAddEnergy(shooterDrone, projectile->weaponInfo->energyRefill);
            }
            // add 1 so we can differentiate between no weapon and weapon 0
            shooterDrone->stepInfo.shotHit[hitDrone->idx] = projectile->weaponInfo->type + 1;
            e->stats[shooterDrone->idx].shotsHit[projectile->weaponInfo->type]++;
            DEBUG_LOGF("drone %d hit drone %d with weapon %d", shooterDrone->idx, hitDrone->idx, projectile->weaponInfo->type);
            hitDrone->stepInfo.shotTaken[shooterDrone->idx] = projectile->weaponInfo->type + 1;
            e->stats[hitDrone->idx].shotsTaken[projectile->weaponInfo->type]++;
            DEBUG_LOGF("drone %d hit by drone %d with weapon %d", hitDrone->idx, shooterDrone->idx, projectile->weaponInfo->type);
        } else {
            hitDrone->stepInfo.ownShotTaken = true;
            e->stats[hitDrone->idx].ownShotsTaken[projectile->weaponInfo->type]++;
            DEBUG_LOGF("drone %d hit by own weapon %d", hitDrone->idx, projectile->weaponInfo->type);
        }

        if (projectile->weaponInfo->destroyedOnDroneHit) {
            destroyProjectile(e, projectile, projectile->weaponInfo->explodesOnDroneHit, true);
            destroyExplodedProjectiles(e);
            return 1;
        }
    } else if (projectile->weaponInfo->type == MINE_LAUNCHER_WEAPON && !projectile->setMine) {
        // if the mine is in explosion proximity of a drone now,
        // destroy it
        const b2QueryFilter filter = {
            .categoryBits = PROJECTILE_SHAPE,
            .maskBits = DRONE_SHAPE,
        };
        if (isOverlappingCircleInLineOfSight(e, projectile->ent, projectile->pos, MINE_LAUNCHER_PROXIMITY_RADIUS, filter, NULL)) {
            destroyProjectile(e, projectile, true, true);
            destroyExplodedProjectiles(e);
            return 1;
        }

        // create a weld joint to stick the mine to the wall
        ASSERT(entityTypeIsWall(ent->type));
        wallEntity *wall = ent->entity;
        ASSERT(manifold->pointCount == 1);

        b2WeldJointDef jointDef = b2DefaultWeldJointDef();
        jointDef.bodyIdA = projectile->bodyID;
        jointDef.bodyIdB = wall->bodyID;
        if (projIsShapeA) {
            jointDef.localAnchorA = manifold->points[0].anchorA;
            jointDef.localAnchorB = manifold->points[0].anchorB;
        } else {
            jointDef.localAnchorA = manifold->points[0].anchorB;
            jointDef.localAnchorB = manifold->points[0].anchorA;
        }
        b2CreateWeldJoint(e->worldID, &jointDef);
        projectile->velocity = b2Vec2_zero;
        projectile->lastVelocity = b2Vec2_zero;
        projectile->speed = 0.0f;
        projectile->lastSpeed = 0.0f;
        projectile->setMine = true;
    }

    const uint8_t maxBounces = projectile->weaponInfo->maxBounces;
    if (projectile->bounces == maxBounces) {
        destroyProjectile(e, projectile, true, true);
        destroyExplodedProjectiles(e);
        return 1;
    }

    return 0;
}

// ensure speed is maintained when a projectile hits a dynamic body
void handleProjectileEndContact(const entity *proj, const entity *ent) {
    projectileEntity *projectile = proj->entity;
    projectile->contacts--;

    // mines stick to walls, explode when hitting another projectile
    // and are destroyed when hitting a drone so no matter what we don't
    // need to do anything here
    if (projectile->weaponInfo->type == MINE_LAUNCHER_WEAPON) {
        return;
    }

    if (ent != NULL) {
        if (ent->type == PROJECTILE_ENTITY) {
            const projectileEntity *projectile2 = ent->entity;
            // allow projectile speeds to increase when two different
            // projectile types collide
            if (projectile->weaponInfo->type != projectile2->weaponInfo->type) {
                b2Vec2 newVel = b2Body_GetLinearVelocity(projectile->bodyID);
                float newSpeed = b2Length(newVel);
                if (newSpeed < projectile->lastSpeed) {
                    newSpeed = projectile->lastSpeed;
                    newVel = b2MulSV(newSpeed, b2Normalize(newVel));
                    b2Body_SetLinearVelocity(projectile->bodyID, newVel);
                }
                projectile->velocity = newVel;
                projectile->speed = newSpeed;
                projectile->lastSpeed = newSpeed;
                return;
            }
        }
    }

    // the last speed is used here instead of the current speed because
    // the current speed will be the speed box2d set the projectile to
    // after a collision and we want to keep the speed consistent
    float newSpeed = projectile->lastSpeed;
    if (projectile->weaponInfo->type == ACCELERATOR_WEAPON) {
        newSpeed = fminf(projectile->lastSpeed * ACCELERATOR_BOUNCE_SPEED_COEF, ACCELERATOR_MAX_SPEED);
        projectile->speed = newSpeed;
    }

    // ensure the projectile's speed doesn't change after bouncing off
    // something
    b2Vec2 newVel = b2Body_GetLinearVelocity(projectile->bodyID);
    newVel = b2MulSV(newSpeed, b2Normalize(newVel));
    b2Body_SetLinearVelocity(projectile->bodyID, newVel);
    projectile->velocity = newVel;
    projectile->speed = newSpeed;
    projectile->lastSpeed = newSpeed;
}

void handleContactEvents(env *e) {
    b2ContactEvents events = b2World_GetContactEvents(e->worldID);
    for (int i = 0; i < events.beginCount; ++i) {
        const b2ContactBeginTouchEvent *event = events.beginEvents + i;
        entity *e1 = NULL;
        entity *e2 = NULL;

        if (b2Shape_IsValid(event->shapeIdA)) {
            e1 = b2Shape_GetUserData(event->shapeIdA);
            ASSERT(e1 != NULL);
        }
        if (b2Shape_IsValid(event->shapeIdB)) {
            e2 = b2Shape_GetUserData(event->shapeIdB);
            ASSERT(e2 != NULL);
        }

        if (e1 != NULL) {
            if (e1->type == PROJECTILE_ENTITY) {
                uint8_t numDestroyed = handleProjectileBeginContact(e, e1, e2, &event->manifold, true);
                if (numDestroyed == 2) {
                    continue;
                } else if (numDestroyed == 1) {
                    e1 = NULL;
                }

            } else if (e1->type == DEATH_WALL_ENTITY && e2 != NULL) {
                if (e2->type == DRONE_ENTITY) {
                    droneEntity *drone = e2->entity;
                    killDrone(e, drone);
                } else if (e2->type == SHIELD_ENTITY) {
                    shieldEntity *shield = e2->entity;
                    destroyDroneShield(shield);
                    e2 = NULL;
                }
            }
        }
        if (e2 != NULL) {
            if (e2->type == PROJECTILE_ENTITY) {
                handleProjectileBeginContact(e, e2, e1, &event->manifold, false);
            } else if (e2->type == DEATH_WALL_ENTITY && e1 != NULL) {
                if (e1->type == DRONE_ENTITY) {
                    droneEntity *drone = e1->entity;
                    killDrone(e, drone);
                } else if (e1->type == SHIELD_ENTITY) {
                    shieldEntity *shield = e1->entity;
                    destroyDroneShield(shield);
                }
            }
        }
    }

    for (int i = 0; i < events.endCount; ++i) {
        const b2ContactEndTouchEvent *event = events.endEvents + i;
        entity *e1 = NULL;
        entity *e2 = NULL;
        if (b2Shape_IsValid(event->shapeIdA)) {
            e1 = b2Shape_GetUserData(event->shapeIdA);
            ASSERT(e1 != NULL);
        }
        if (b2Shape_IsValid(event->shapeIdB)) {
            e2 = b2Shape_GetUserData(event->shapeIdB);
            ASSERT(e2 != NULL);
        }
        if (e1 != NULL && e1->type == PROJECTILE_ENTITY) {
            handleProjectileEndContact(e1, e2);
        }
        if (e2 != NULL && e2->type == PROJECTILE_ENTITY) {
            handleProjectileEndContact(e2, e1);
        }
    }
}

// set pickup to respawn somewhere else randomly if a drone touched it,
// mark the pickup as disabled if a floating wall is touching it
void handleWeaponPickupBeginTouch(env *e, const entity *sensor, entity *visitor) {
    weaponPickupEntity *pickup = sensor->entity;
    if (pickup->floatingWallsTouching != 0) {
        return;
    }

    wallEntity *wall;

    switch (visitor->type) {
    case DRONE_ENTITY:
        disableWeaponPickup(e, pickup);

        droneEntity *drone = visitor->entity;
        drone->stepInfo.pickedUpWeapon = true;
        drone->stepInfo.prevWeapon = drone->weaponInfo->type;
        droneChangeWeapon(e, drone, pickup->weapon);

        e->stats[drone->idx].weaponsPickedUp[pickup->weapon]++;
        DEBUG_LOGF("drone %d picked up weapon %d", drone->idx, pickup->weapon);
        break;
    case STANDARD_WALL_ENTITY:
    case BOUNCY_WALL_ENTITY:
    case DEATH_WALL_ENTITY:
        wall = visitor->entity;
        if (!wall->isFloating) {
            if (!wall->isSuddenDeath) {
                ERRORF("non sudden death wall type %d at cell %d touched weapon pickup", visitor->type, wall->mapCellIdx);
            }
            return;
        }

        pickup->floatingWallsTouching++;
        break;
    default:
        ERRORF("invalid weapon pickup begin touch visitor %d", visitor->type);
    }
}

// explode proximity detonating projectiles
void handleProjectileBeginTouch(env *e, const entity *sensor, const entity *visitor) {
    projectileEntity *projectile = sensor->entity;

    switch (projectile->weaponInfo->type) {
    case FLAK_CANNON_WEAPON:
        if (projectile->distance < FLAK_CANNON_SAFE_DISTANCE) {
            return;
        }
        destroyProjectile(e, projectile, true, true);
        destroyExplodedProjectiles(e);
        break;
    case MINE_LAUNCHER_WEAPON:
        if (!projectile->setMine) {
            return;
        }

        ASSERT(visitor->type == DRONE_ENTITY);
        const b2DistanceOutput output = closestPoint(sensor, visitor);
        const b2QueryFilter filter = {.categoryBits = PROJECTILE_SHAPE, .maskBits = WALL_SHAPE | FLOATING_WALL_SHAPE};
        if (posBehindWall(e, projectile->pos, output.pointB, NULL, filter, NULL)) {
            const droneEntity *drone = visitor->entity;
            projectile->dronesBehindWalls[projectile->numDronesBehindWalls++] = drone->idx;
            return;
        }

        destroyProjectile(e, projectile, true, true);
        destroyExplodedProjectiles(e);
        break;
    default:
        ERRORF("invalid projectile type %d for begin touch event", sensor->type);
    }
}

// mark the pickup as enabled if no floating walls are touching it
void handleWeaponPickupEndTouch(const entity *sensor, entity *visitor) {
    weaponPickupEntity *pickup = sensor->entity;
    if (pickup->respawnWait != 0.0f) {
        return;
    }

    wallEntity *wall;

    switch (visitor->type) {
    case DRONE_ENTITY:
        break;
    case STANDARD_WALL_ENTITY:
    case BOUNCY_WALL_ENTITY:
    case DEATH_WALL_ENTITY:
        wall = visitor->entity;
        if (!wall->isFloating) {
            return;
        }

        pickup->floatingWallsTouching--;
        break;
    default:
        ERRORF("invalid weapon pickup end touch visitor %d", visitor->type);
    }
}

void handleProjectileEndTouch(const entity *sensor) {
    projectileEntity *projectile = sensor->entity;
    if (projectile->numDronesBehindWalls == 0 || projectile->weaponInfo->type != MINE_LAUNCHER_WEAPON) {
        return;
    }
    projectile->numDronesBehindWalls--;
}

void handleSensorEvents(env *e) {
    b2SensorEvents events = b2World_GetSensorEvents(e->worldID);
    for (int i = 0; i < events.beginCount; ++i) {
        const b2SensorBeginTouchEvent *event = events.beginEvents + i;
        if (!b2Shape_IsValid(event->sensorShapeId)) {
            DEBUG_LOG("could not find sensor shape for begin touch event");
            continue;
        }
        entity *s = b2Shape_GetUserData(event->sensorShapeId);
        ASSERT(s != NULL);

        if (!b2Shape_IsValid(event->visitorShapeId)) {
            DEBUG_LOG("could not find visitor shape for begin touch event");
            continue;
        }
        entity *v = b2Shape_GetUserData(event->visitorShapeId);
        ASSERT(v != NULL);

        switch (s->type) {
        case WEAPON_PICKUP_ENTITY:
            handleWeaponPickupBeginTouch(e, s, v);
            break;
        case PROJECTILE_ENTITY:
            handleProjectileBeginTouch(e, s, v);
            break;
        default:
            ERRORF("unknown entity type %d for sensor begin touch event", s->type);
        }
    }

    for (int i = 0; i < events.endCount; ++i) {
        const b2SensorEndTouchEvent *event = events.endEvents + i;
        if (!b2Shape_IsValid(event->sensorShapeId)) {
            DEBUG_LOG("could not find sensor shape for end touch event");
            continue;
        }
        entity *s = b2Shape_GetUserData(event->sensorShapeId);
        ASSERT(s != NULL);
        if (s->type == PROJECTILE_ENTITY) {
            handleProjectileEndTouch(s);
            continue;
        }

        if (!b2Shape_IsValid(event->visitorShapeId)) {
            DEBUG_LOG("could not find visitor shape for end touch event");
            continue;
        }
        entity *v = b2Shape_GetUserData(event->visitorShapeId);
        ASSERT(v != NULL);

        handleWeaponPickupEndTouch(s, v);
    }
}

void findNearWalls(const env *e, const droneEntity *drone, nearEntity nearestWalls[], const uint8_t nWalls) {
    nearEntity nearWalls[MAX_NEAREST_WALLS];

    for (uint8_t i = 0; i < MAX_NEAREST_WALLS; ++i) {
        const uint32_t idx = (MAX_NEAREST_WALLS * drone->mapCellIdx) + i;
        const uint16_t wallIdx = e->map->nearestWalls[idx].idx;
        wallEntity *wall = safe_array_get_at(e->walls, wallIdx);
        nearWalls[i].entity = wall;
        nearWalls[i].distanceSquared = b2DistanceSquared(drone->pos, wall->pos);
    }
    insertionSort(nearWalls, MAX_NEAREST_WALLS);
    memcpy(nearestWalls, nearWalls, nWalls * sizeof(nearEntity));
}

#endif
