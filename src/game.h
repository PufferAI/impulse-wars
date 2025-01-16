#ifndef IMPULSE_WARS_GAME_H
#define IMPULSE_WARS_GAME_H

#include "env.h"
#include "helpers.h"
#include "settings.h"
#include "types.h"

static inline bool entityTypeIsWall(const enum entityType type) {
    return type == STANDARD_WALL_ENTITY || type == BOUNCY_WALL_ENTITY || type == DEATH_WALL_ENTITY;
}

static inline b2Vec2 getCachedPos(const b2BodyId bodyID, cachedPos *pos) {
    if (pos->valid) {
        return pos->pos;
    }
    pos->pos = b2Body_GetPosition(bodyID);
    pos->valid = true;
    return pos->pos;
}

static inline int16_t cellIndex(const env *e, const int8_t row, const int8_t col) {
    return row + (col * e->columns);
}

static inline int16_t entityPosToCellIdx(const env *e, const b2Vec2 pos) {
    const float cellX = pos.x + (((float)e->columns * WALL_THICKNESS) / 2.0f);
    const float cellY = pos.y + (((float)e->rows * WALL_THICKNESS) / 2.0f);
    const uint8_t cellRow = cellX / WALL_THICKNESS;
    const uint8_t cellCol = cellY / WALL_THICKNESS;
    const int16_t cellIdx = cellIndex(e, cellRow, cellCol);
    // set the cell to -1 if it's out of bounds
    // TODO: this is a box2d issue, investigate more
    if (cellIdx < 0 || (uint16_t)cellIdx >= cc_array_size(e->cells)) {
        DEBUG_LOGF("invalid cell index: %d from position: (%f, %f)", cellIdx, pos.x, pos.y);
        return -1;
    }
    return cellIdx;
}

bool overlapCallback(b2ShapeId shapeID, void *context) {
    // the b2ShapeId parameter is required to match the prototype of the callback function
    MAYBE_UNUSED(shapeID);

    bool *overlaps = (bool *)context;
    *overlaps = true;
    return false;
}

// returns true if the given position overlaps with a bounding box with
// a height and width of distance with shape categories specified in maskBits
bool isOverlapping(env *e, const b2Vec2 pos, const float distance, const enum shapeCategory type, const uint64_t maskBits) {
    b2AABB bounds = {
        .lowerBound = {.x = pos.x - distance, .y = pos.y - distance},
        .upperBound = {.x = pos.x + distance, .y = pos.y + distance},
    };
    b2QueryFilter filter = {
        .categoryBits = type,
        .maskBits = maskBits,
    };
    bool overlaps = false;
    b2World_OverlapAABB(e->worldID, bounds, filter, overlapCallback, &overlaps);
    return overlaps;
}

// returns true and sets emptyPos to the position of an empty cell
// that is an appropriate distance away from other entities if one exists
bool findOpenPos(env *e, const enum shapeCategory type, b2Vec2 *emptyPos) {
    uint8_t checkedCells[BITNSLOTS(MAX_CELLS)] = {0};
    const size_t nCells = cc_array_size(e->cells);
    uint16_t attempts = 0;

    while (true) {
        if (attempts == nCells) {
            return false;
        }
        const int cellIdx = randInt(&e->randState, 0, nCells - 1);
        if (bitTest(checkedCells, cellIdx)) {
            continue;
        }
        bitSet(checkedCells, cellIdx);
        attempts++;

        const mapCell *cell = safe_array_get_at(e->cells, cellIdx);
        if (cell->ent != NULL) {
            continue;
        }

        // ensure drones don't spawn too close to walls or other drones
        if (type == DRONE_SHAPE) {
            if (isOverlapping(e, cell->pos, DRONE_WALL_SPAWN_DISTANCE, DRONE_SHAPE, WALL_SHAPE | DRONE_SHAPE)) {
                continue;
            }
            if (isOverlapping(e, cell->pos, DRONE_DRONE_SPAWN_DISTANCE, DRONE_SHAPE, DRONE_SHAPE)) {
                continue;
            }
        }

        if (!isOverlapping(e, cell->pos, MIN_SPAWN_DISTANCE, type, FLOATING_WALL_SHAPE | WEAPON_PICKUP_SHAPE | DRONE_SHAPE)) {
            *emptyPos = cell->pos;
            return true;
        }
    }
}

entity *createWall(env *e, const float posX, const float posY, const float width, const float height, uint16_t cellIdx, const enum entityType type, const bool floating) {
    ASSERT(entityTypeIsWall(type));

    if (floating) {
        cellIdx = -1;
    }
    const b2Vec2 pos = (b2Vec2){.x = posX, .y = posY};

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
    wallShapeDef.restitution = 0.1f;
    wallShapeDef.filter.categoryBits = WALL_SHAPE;
    wallShapeDef.filter.maskBits = FLOATING_WALL_SHAPE | PROJECTILE_SHAPE | WEAPON_PICKUP_SHAPE | DRONE_SHAPE;
    if (floating) {
        wallShapeDef.filter.categoryBits = FLOATING_WALL_SHAPE;
        wallShapeDef.filter.maskBits |= WALL_SHAPE | WEAPON_PICKUP_SHAPE;
        wallShapeDef.enableSensorEvents = true;
    }

    if (type == BOUNCY_WALL_ENTITY) {
        wallShapeDef.restitution = BOUNCY_WALL_RESTITUTION;
    }
    if (type == DEATH_WALL_ENTITY) {
        wallShapeDef.enableContactEvents = true;
    }

    wallEntity *wall = (wallEntity *)fastMalloc(sizeof(wallEntity));
    wall->bodyID = wallBodyID;
    wall->pos = (cachedPos){.pos = pos, .valid = true};
    wall->extent = extent;
    wall->mapCellIdx = cellIdx;
    wall->isFloating = floating;
    wall->type = type;
    wall->isSuddenDeath = e->suddenDeathSteps == 0;

    entity *ent = (entity *)fastMalloc(sizeof(entity));
    ent->type = type;
    ent->entity = wall;

    wallShapeDef.userData = ent;
    const b2Polygon wallPolygon = b2MakeBox(extent.x, extent.y);
    wall->shapeID = b2CreatePolygonShape(wallBodyID, &wallShapeDef, &wallPolygon);

    if (floating) {
        cc_array_add(e->floatingWalls, wall);
    } else {
        cc_array_add(e->walls, wall);
        // DEBUG_LOGF("adding wall at (%f, %f) to KD tree", pos.x, pos.y);
        kd_insert(e->wallTree, pos.x, pos.y, wall);
    }

    return ent;
}

void destroyWall(wallEntity *wall) {
    entity *ent = (entity *)b2Shape_GetUserData(wall->shapeID);
    fastFree(ent);

    b2DestroyBody(wall->bodyID);
    fastFree(wall);
}

void createSuddenDeathWalls(env *e, const b2Vec2 startPos, const b2Vec2 size) {
    int16_t endIdx;
    uint8_t indexIncrement;
    if (size.y == WALL_THICKNESS) {
        const b2Vec2 endPos = (b2Vec2){.x = startPos.x + size.x, .y = startPos.y};
        endIdx = entityPosToCellIdx(e, endPos);
        if (endIdx == -1) {
            ERRORF("invalid position for sudden death wall: (%f, %f)", endPos.x, endPos.y);
        }
        indexIncrement = 1;
    } else {
        const b2Vec2 endPos = (b2Vec2){.x = startPos.x, .y = startPos.y + size.y};
        endIdx = entityPosToCellIdx(e, endPos);
        if (endIdx == -1) {
            ERRORF("invalid position for sudden death wall: (%f, %f)", endPos.x, endPos.y);
        }
        indexIncrement = e->rows;
    }
    const int16_t startIdx = entityPosToCellIdx(e, startPos);
    if (startIdx == -1) {
        ERRORF("invalid position for sudden death wall: (%f, %f)", startPos.x, startPos.y);
    }
    for (uint16_t i = startIdx; i <= endIdx; i += indexIncrement) {
        mapCell *cell = safe_array_get_at(e->cells, i);
        if (cell->ent != NULL && cell->ent->type == WEAPON_PICKUP_ENTITY) {
            weaponPickupEntity *pickup = (weaponPickupEntity *)cell->ent->entity;
            pickup->respawnWait = PICKUP_RESPAWN_WAIT;
        }
        entity *ent = createWall(e, cell->pos.x, cell->pos.y, WALL_THICKNESS, WALL_THICKNESS, i, DEATH_WALL_ENTITY, false);
        cell->ent = ent;
    }
}

b2ShapeProxy makeDistanceProxy(const entity *ent, bool *isCircle) {
    b2ShapeProxy proxy = {0};
    switch (ent->type) {
    case PROJECTILE_ENTITY:
        *isCircle = true;
        const projectileEntity *proj = (projectileEntity *)ent->entity;
        proxy.radius = proj->weaponInfo->radius;
        break;
    case DRONE_ENTITY:
        *isCircle = true;
        proxy.radius = DRONE_RADIUS;
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
        ERRORF("unknown entity type for shape distance: %d", ent->type);
    }

    return proxy;
}

enum weaponType randWeaponPickupType(env *e) {
    while (true) {
        const enum weaponType type = (enum weaponType)randInt(&e->randState, STANDARD_WEAPON + 1, NUM_WEAPONS - 1);
        if (type != e->defaultWeapon->type) {
            return type;
        }
    }
}

void createWeaponPickup(env *e) {
    b2BodyDef pickupBodyDef = b2DefaultBodyDef();
    b2Vec2 pos;
    if (!findOpenPos(e, WEAPON_PICKUP_SHAPE, &pos)) {
        ERROR("no open position for weapon pickup");
    }
    pickupBodyDef.position = pos;
    b2BodyId pickupBodyID = b2CreateBody(e->worldID, &pickupBodyDef);
    b2ShapeDef pickupShapeDef = b2DefaultShapeDef();
    pickupShapeDef.filter.categoryBits = WEAPON_PICKUP_SHAPE;
    pickupShapeDef.filter.maskBits = WALL_SHAPE | FLOATING_WALL_SHAPE | WEAPON_PICKUP_SHAPE | DRONE_SHAPE;
    pickupShapeDef.isSensor = true;

    weaponPickupEntity *pickup = (weaponPickupEntity *)fastMalloc(sizeof(weaponPickupEntity));
    pickup->bodyID = pickupBodyID;
    pickup->weapon = randWeaponPickupType(e);
    pickup->respawnWait = 0.0f;
    pickup->floatingWallsTouching = 0;
    pickup->pos = pos;

    entity *ent = (entity *)fastMalloc(sizeof(entity));
    ent->type = WEAPON_PICKUP_ENTITY;
    ent->entity = pickup;

    const int16_t cellIdx = entityPosToCellIdx(e, pos);
    if (cellIdx == -1) {
        ERRORF("invalid position for weapon pickup spawn: (%f, %f)", pos.x, pos.y);
    }
    pickup->mapCellIdx = cellIdx;
    mapCell *cell = safe_array_get_at(e->cells, cellIdx);
    cell->ent = ent;

    pickupShapeDef.userData = ent;
    const b2Polygon pickupPolygon = b2MakeBox(PICKUP_THICKNESS / 2.0f, PICKUP_THICKNESS / 2.0f);
    pickup->shapeID = b2CreatePolygonShape(pickupBodyID, &pickupShapeDef, &pickupPolygon);

    cc_array_add(e->pickups, pickup);
    // kd_insert(e->pickupTree, pos.x, pos.y, pickup);
}

void destroyWeaponPickup(env *e, weaponPickupEntity *pickup, const bool full) {
    entity *ent = (entity *)b2Shape_GetUserData(pickup->shapeID);
    fastFree(ent);

    if (full) {
        MAYBE_UNUSED(e);
        // const bool deleted = kd_delete(e->pickupTree, pickup->pos.x, pickup->pos.y);
        // MAYBE_UNUSED(deleted);
        // ASSERT(deleted);
        b2DestroyBody(pickup->bodyID);
    }

    fastFree(pickup);
}

void createDrone(env *e, const uint8_t idx) {
    b2BodyDef droneBodyDef = b2DefaultBodyDef();
    droneBodyDef.type = b2_dynamicBody;
    if (!findOpenPos(e, DRONE_SHAPE, &droneBodyDef.position)) {
        ERROR("no open position for drone");
    }
    droneBodyDef.fixedRotation = true;
    droneBodyDef.linearDamping = DRONE_LINEAR_DAMPING;
    b2BodyId droneBodyID = b2CreateBody(e->worldID, &droneBodyDef);
    b2ShapeDef droneShapeDef = b2DefaultShapeDef();
    droneShapeDef.density = DRONE_DENSITY;
    droneShapeDef.friction = 0.0f;
    droneShapeDef.restitution = 0.3f;
    droneShapeDef.filter.categoryBits = DRONE_SHAPE;
    droneShapeDef.filter.maskBits = WALL_SHAPE | FLOATING_WALL_SHAPE | WEAPON_PICKUP_SHAPE | PROJECTILE_SHAPE | DRONE_SHAPE;
    droneShapeDef.enableContactEvents = true;
    droneShapeDef.enableSensorEvents = true;
    const b2Circle droneCircle = {.center = b2Vec2_zero, .radius = DRONE_RADIUS};

    droneEntity *drone = (droneEntity *)fastMalloc(sizeof(droneEntity));
    drone->bodyID = droneBodyID;
    drone->weaponInfo = e->defaultWeapon;
    drone->ammo = weaponAmmo(e->defaultWeapon->type, drone->weaponInfo->type);
    drone->weaponCooldown = 0.0f;
    drone->heat = 0;
    drone->charge = 0;
    drone->energyLeft = DRONE_ENERGY_MAX;
    drone->lightBraking = false;
    drone->heavyBraking = false;
    drone->chargingBurst = false;
    drone->burstCharge = 0.0f;
    drone->energyFullyDepleted = false;
    drone->energyFullyDepletedThisStep = false;
    drone->energyRefillWait = 0;
    drone->shotThisStep = false;
    drone->idx = idx;
    drone->initalPos = droneBodyDef.position;
    drone->pos = (cachedPos){.pos = droneBodyDef.position, .valid = true};
    drone->lastPos = b2Vec2_zero;
    drone->lastMove = b2Vec2_zero;
    drone->lastAim = (b2Vec2){.x = 0.0f, .y = -1.0f};
    drone->lastVelocity = b2Vec2_zero;
    drone->dead = false;
    memset(&drone->stepInfo, 0x0, sizeof(droneStepInfo));
    memset(&drone->inLineOfSight, 0x0, sizeof(drone->inLineOfSight));

    entity *ent = (entity *)fastMalloc(sizeof(entity));
    ent->type = DRONE_ENTITY;
    ent->entity = drone;

    droneShapeDef.userData = ent;
    drone->shapeID = b2CreateCircleShape(droneBodyID, &droneShapeDef, &droneCircle);

    cc_array_add(e->drones, drone);
}

void destroyDrone(droneEntity *drone) {
    entity *ent = (entity *)b2Shape_GetUserData(drone->shapeID);
    fastFree(ent);

    b2DestroyBody(drone->bodyID);
    fastFree(drone);
}

void createProjectile(env *e, droneEntity *drone, const b2Vec2 normAim) {
    ASSERT_VEC_NORMALIZED(normAim);

    b2BodyDef projectileBodyDef = b2DefaultBodyDef();
    projectileBodyDef.type = b2_dynamicBody;
    projectileBodyDef.fixedRotation = true;
    projectileBodyDef.isBullet = drone->weaponInfo->isPhysicsBullet;
    projectileBodyDef.enableSleep = false;
    b2Vec2 dronePos = getCachedPos(drone->bodyID, &drone->pos);
    float radius = drone->weaponInfo->radius;
    projectileBodyDef.position = b2MulAdd(dronePos, 1.0f + (radius * 1.5f), normAim);
    b2BodyId projectileBodyID = b2CreateBody(e->worldID, &projectileBodyDef);
    b2ShapeDef projectileShapeDef = b2DefaultShapeDef();
    projectileShapeDef.enableContactEvents = true;
    projectileShapeDef.density = drone->weaponInfo->density;
    projectileShapeDef.friction = 0.0f;
    projectileShapeDef.restitution = 1.0f;
    projectileShapeDef.filter.categoryBits = PROJECTILE_SHAPE;
    projectileShapeDef.filter.maskBits = WALL_SHAPE | FLOATING_WALL_SHAPE | PROJECTILE_SHAPE | DRONE_SHAPE;
    const b2Circle projectileCircle = {.center = b2Vec2_zero, .radius = radius};

    b2ShapeId projectileShapeID = b2CreateCircleShape(projectileBodyID, &projectileShapeDef, &projectileCircle);

    // add lateral drone velocity to projectile
    b2Vec2 droneVel = b2Body_GetLinearVelocity(drone->bodyID);
    b2Vec2 forwardVel = b2MulSV(b2Dot(droneVel, normAim), normAim);
    b2Vec2 lateralVel = b2Sub(droneVel, forwardVel);
    lateralVel = b2MulSV(projectileShapeDef.density / DRONE_MOVE_AIM_DIVISOR, lateralVel);
    b2Vec2 aim = weaponAdjustAim(&e->randState, drone->weaponInfo->type, drone->heat, normAim);
    b2Vec2 fire = b2MulAdd(lateralVel, weaponFire(&e->randState, drone->weaponInfo->type), aim);
    b2Body_ApplyLinearImpulseToCenter(projectileBodyID, fire, true);

    projectileEntity *projectile = (projectileEntity *)fastMalloc(sizeof(projectileEntity));
    projectile->droneIdx = drone->idx;
    projectile->bodyID = projectileBodyID;
    projectile->shapeID = projectileShapeID;
    projectile->weaponInfo = drone->weaponInfo;
    projectile->lastPos = projectileBodyDef.position;
    projectile->distance = 0.0f;
    projectile->bounces = 0;
    cc_slist_add(e->projectiles, projectile);

    entity *ent = (entity *)fastMalloc(sizeof(entity));
    ent->type = PROJECTILE_ENTITY;
    ent->entity = projectile;

    b2Shape_SetUserData(projectile->shapeID, ent);
}

typedef struct explosionCallbackContext {
    droneEntity *drone;
    env *e;
    enum weaponType weaponType;
} explosionCallbackContext;

bool explosionOverlapCallback(b2ShapeId shapeId, void *context) {
    explosionCallbackContext *ctx = (explosionCallbackContext *)context;
    entity *ent = (entity *)b2Shape_GetUserData(shapeId);
    droneEntity *hitDrone = (droneEntity *)ent->entity;
    if (hitDrone->idx == ctx->drone->idx) {
        hitDrone->stepInfo.ownShotTaken = true;
        ctx->e->stats[hitDrone->idx].ownShotsTaken[ctx->weaponType]++;
        DEBUG_LOGF("drone %d hit itself with explosion from weapon %d", ctx->drone->idx, ctx->weaponType);
        return true;
    }

    ctx->drone->stepInfo.explosionHit[hitDrone->idx] = true;
    ctx->e->stats[ctx->drone->idx].shotsHit[ctx->weaponType]++;
    DEBUG_LOGF("drone %d hit drone %d with explosion from weapon %d", ctx->drone->idx, hitDrone->idx, ctx->weaponType);
    hitDrone->stepInfo.shotTaken[ctx->drone->idx] = true;
    ctx->e->stats[hitDrone->idx].shotsTaken[ctx->weaponType]++;
    DEBUG_LOGF("drone %d hit by explosion from drone %d with weapon %d", hitDrone->idx, ctx->drone->idx, ctx->weaponType);

    return true;
}

void destroyProjectile(env *e, projectileEntity *projectile, const bool full) {
    // explode projectile if necessary
    b2ExplosionDef explosion;
    if (full && weaponExplosion(projectile->weaponInfo->type, &explosion)) {
        const b2Vec2 pos = getCachedPos(projectile->bodyID, &projectile->pos);
        explosion.position = pos;
        explosion.maskBits = FLOATING_WALL_SHAPE | DRONE_SHAPE;
        b2World_Explode(e->worldID, &explosion);

        if (e->client != NULL) {
            explosionInfo *explInfo = fastMalloc(sizeof(explosionInfo));
            explInfo->def = explosion;
            explInfo->renderSteps = UINT16_MAX;
            cc_array_add(e->explosions, explInfo);
        }

        // check if enemy drone is in explosion radius
        const float totalRadius = explosion.radius + explosion.falloff;
        const b2Circle cir = {.center = b2Vec2_zero, .radius = totalRadius};
        const b2Transform transform = {.p = pos, .q = b2Rot_identity};
        const b2QueryFilter filter = {
            .categoryBits = PROJECTILE_SHAPE,
            .maskBits = DRONE_SHAPE,
        };
        droneEntity *drone = safe_array_get_at(e->drones, projectile->droneIdx);
        explosionCallbackContext ctx = {
            .drone = drone,
            .e = e,
            .weaponType = projectile->weaponInfo->type,
        };
        b2World_OverlapCircle(e->worldID, &cir, transform, filter, explosionOverlapCallback, &ctx);
    }

    entity *ent = (entity *)b2Shape_GetUserData(projectile->shapeID);
    fastFree(ent);

    if (full) {
        DEBUG_LOGF("proj %d speed: %f", projectile->weaponInfo->type, b2Length(b2Body_GetLinearVelocity(projectile->bodyID)));

        const enum cc_stat res = cc_slist_remove(e->projectiles, projectile, NULL);
        MAYBE_UNUSED(res);
        ASSERT(res == CC_OK);
        b2DestroyBody(projectile->bodyID);
    } else {
        // only add to the stats if we are not clearing the environment,
        // otherwise this projectile's distance will be counted twice
        e->stats[projectile->droneIdx].shotDistances[projectile->droneIdx] += projectile->distance;
    }

    fastFree(projectile);
}

void destroyAllProjectiles(env *e) {
    for (SNode *cur = e->projectiles->head; cur != NULL; cur = cur->next) {
        projectileEntity *p = (projectileEntity *)cur->data;
        destroyProjectile(e, p, false);
    }
}

void handleSuddenDeath(env *e) {
    ASSERT(e->suddenDeathSteps == 0);

    // create new walls that will close in on the arena
    e->suddenDeathWallCounter++;

    // TODO: these magic numbers can probably be simplified somehow
    createSuddenDeathWalls(
        e,
        (b2Vec2){
            .x = e->bounds.min.x + ((e->suddenDeathWallCounter - 1) * WALL_THICKNESS),
            .y = e->bounds.min.y + ((WALL_THICKNESS * (e->suddenDeathWallCounter - 1)) + (WALL_THICKNESS / 2)),
        },
        (b2Vec2){
            .x = WALL_THICKNESS * (e->columns - (e->suddenDeathWallCounter * 2) - 1),
            .y = WALL_THICKNESS,
        });
    createSuddenDeathWalls(
        e,
        (b2Vec2){
            .x = e->bounds.min.x + ((e->suddenDeathWallCounter - 1) * WALL_THICKNESS),
            .y = e->bounds.max.y - ((WALL_THICKNESS * (e->suddenDeathWallCounter - 1)) + (WALL_THICKNESS / 2)),
        },
        (b2Vec2){
            .x = WALL_THICKNESS * (e->columns - (e->suddenDeathWallCounter * 2) - 1),
            .y = WALL_THICKNESS,
        });
    createSuddenDeathWalls(
        e,
        (b2Vec2){
            .x = e->bounds.min.x + ((e->suddenDeathWallCounter - 1) * WALL_THICKNESS),
            .y = e->bounds.min.y + (e->suddenDeathWallCounter * WALL_THICKNESS),
        },
        (b2Vec2){
            .x = WALL_THICKNESS,
            .y = WALL_THICKNESS * (e->rows - (e->suddenDeathWallCounter * 2) - 2),
        });
    createSuddenDeathWalls(
        e,
        (b2Vec2){
            .x = e->bounds.min.x + ((e->columns - e->suddenDeathWallCounter - 2) * WALL_THICKNESS),
            .y = e->bounds.min.y + (e->suddenDeathWallCounter * WALL_THICKNESS),
        },
        (b2Vec2){
            .x = WALL_THICKNESS,
            .y = WALL_THICKNESS * (e->rows - (e->suddenDeathWallCounter * 2) - 2),
        });

    // mark drones as dead if they touch a newly placed wall
    bool droneDead = false;
    for (uint8_t i = 0; i < e->numDrones; i++) {
        droneEntity *drone = safe_array_get_at(e->drones, i);
        const b2Vec2 pos = getCachedPos(drone->bodyID, &drone->pos);
        if (isOverlapping(e, pos, DRONE_RADIUS, DRONE_SHAPE, WALL_SHAPE)) {
            drone->dead = true;
            droneDead = true;
        }
    }
    if (droneDead) {
        return;
    }

    // TODO: not all floating walls are destroyed correctly
    // make floating walls static bodies if they are now overlapping with
    // a newly placed wall, but destroy them if they are fully inside a wall
    CC_ArrayIter floatingWallIter;
    cc_array_iter_init(&floatingWallIter, e->floatingWalls);
    wallEntity *wall;
    while (cc_array_iter_next(&floatingWallIter, (void **)&wall) != CC_ITER_END) {
        const b2Vec2 pos = getCachedPos(wall->bodyID, &wall->pos);
        int16_t cellIdx = entityPosToCellIdx(e, pos);
        if (cellIdx == -1) {
            ERRORF("floating wall is out of bounds at %f, %f", pos.x, pos.y);
        }

        const mapCell *cell = safe_array_get_at(e->cells, cellIdx);
        if (cell->ent != NULL && entityTypeIsWall(cell->ent->type)) {
            // floating wall is overlapping with a wall, destroy it
            const enum cc_stat res = cc_array_iter_remove(&floatingWallIter, NULL);
            MAYBE_UNUSED(res);
            ASSERT(res == CC_OK);
            destroyWall(wall);

            DEBUG_LOGF("destroyed floating wall at %f, %f", pos.x, pos.y);
            continue;
        }
    }

    // detroy all projectiles that are now overlapping with a newly placed wall
    CC_SListIter projectileIter;
    cc_slist_iter_init(&projectileIter, e->projectiles);
    projectileEntity *projectile;
    while (cc_slist_iter_next(&projectileIter, (void **)&projectile) != CC_ITER_END) {
        const b2Vec2 pos = getCachedPos(projectile->bodyID, &projectile->pos);
        const int16_t cellIdx = entityPosToCellIdx(e, pos);
        if (cellIdx == -1) {
            continue;
        }
        const mapCell *cell = safe_array_get_at(e->cells, cellIdx);
        if (cell->ent != NULL && entityTypeIsWall(cell->ent->type)) {
            // we have to destroy the projectile using the iterator so
            // we can continue to iterate correctly, copy the body ID
            // so we can destroy it after the projectile has been freed
            cc_slist_iter_remove(&projectileIter, NULL);
            const b2BodyId bodyID = projectile->bodyID;
            destroyProjectile(e, projectile, false);
            b2DestroyBody(bodyID);
        }
    }
}

void droneMove(droneEntity *drone, b2Vec2 direction) {
    ASSERT_VEC_BOUNDED(direction);

    // if energy is fully depleted half movement until energy starts
    // to refill again
    if (drone->energyFullyDepleted && drone->energyRefillWait != 0.0f) {
        direction = b2MulSV(0.5f, direction);
        drone->lastMove = direction;
    }
    b2Vec2 force = b2MulSV(DRONE_MOVE_MAGNITUDE, direction);
    b2Body_ApplyForceToCenter(drone->bodyID, force, true);
}

void droneChangeWeapon(const env *e, droneEntity *drone, const enum weaponType newWeapon) {
    // only top up ammo if the weapon is the same
    if (drone->weaponInfo->type != newWeapon) {
        drone->weaponCooldown = 0.0f;
        drone->charge = 0;
        drone->heat = 0;
    }
    drone->weaponInfo = weaponInfos[newWeapon];
    drone->ammo = weaponAmmo(e->defaultWeapon->type, drone->weaponInfo->type);
}

void droneShoot(env *e, droneEntity *drone, const b2Vec2 aim) {
    ASSERT(drone->ammo != 0);

    drone->shotThisStep = true;
    // TODO: rework heat to only increase when projectiles are fired,
    // and only cool down after the next shot was skipped
    drone->heat++;
    if (drone->weaponCooldown != 0.0f) {
        return;
    }
    drone->charge++;
    if (drone->charge < weaponCharge(e, drone->weaponInfo->type)) {
        return;
    }

    if (drone->ammo != INFINITE) {
        drone->ammo--;
    }
    drone->weaponCooldown = drone->weaponInfo->coolDown;
    drone->charge = 0.0f;

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
        DEBUG_LOGF("drone %d fired %d weapon", drone->idx, drone->weaponInfo->type);
    }
    drone->stepInfo.firedShot = true;

    if (drone->ammo == 0) {
        droneChangeWeapon(e, drone, e->defaultWeapon->type);
        drone->weaponCooldown = drone->weaponInfo->coolDown;
    }
}

void droneBrake(env *e, droneEntity *drone, const bool lightBrake, const bool heavyBrake) {
    // if the drone isn't braking or energy is fully depleted, return
    // unless the drone was braking during the last step
    if ((!lightBrake && !heavyBrake) || drone->energyFullyDepleted) {
        if (drone->lightBraking || drone->heavyBraking) {
            drone->lightBraking = false;
            drone->heavyBraking = false;
            b2Body_SetLinearDamping(drone->bodyID, DRONE_LINEAR_DAMPING);
            if (drone->energyRefillWait == 0.0f && !drone->chargingBurst) {
                drone->energyRefillWait = DRONE_ENERGY_REFILL_WAIT * e->frameRate;
            }
        }
        return;
    }
    ASSERT(!drone->energyFullyDepleted);

    // apply additional brake damping and decrease energy
    if (lightBrake) {
        if (!drone->lightBraking) {
            drone->lightBraking = true;
            b2Body_SetLinearDamping(drone->bodyID, DRONE_LINEAR_DAMPING * DRONE_LIGHT_BRAKE_COEF);
        }
        drone->energyLeft = fmaxf(drone->energyLeft - (DRONE_LIGHT_BRAKE_DRAIN_RATE * e->deltaTime), 0.0f);
        e->stats[drone->idx].lightBrakeTime += e->deltaTime;
    } else if (heavyBrake) {
        if (!drone->heavyBraking) {
            drone->heavyBraking = true;
            b2Body_SetLinearDamping(drone->bodyID, DRONE_LINEAR_DAMPING * DRONE_HEAVY_BRAKE_COEF);
        }
        drone->energyLeft = fmaxf(drone->energyLeft - (DRONE_HEAVY_BRAKE_DRAIN_RATE * e->deltaTime), 0.0f);
        e->stats[drone->idx].heavyBrakeTime += e->deltaTime;
    }

    // if energy is empty but burst is being charged, let burst functions
    // handle energy refill
    if (drone->energyLeft == 0.0f && !drone->chargingBurst) {
        drone->energyFullyDepleted = true;
        drone->energyFullyDepletedThisStep = true;
        drone->energyRefillWait = DRONE_ENERGY_REFILL_EMPTY_WAIT * (float)e->frameRate;
        e->stats[drone->idx].energyEmptied++;
    }

    if (e->client != NULL) {
        brakeTrailPoint *trailPoint = fastMalloc(sizeof(brakeTrailPoint));
        trailPoint->pos = drone->pos.pos;
        trailPoint->heavyBrake = heavyBrake;
        trailPoint->lifetime = UINT16_MAX;
        cc_array_add(e->brakeTrailPoints, trailPoint);
    }
}

void droneChargeBurst(env *e, droneEntity *drone) {
    if (drone->energyFullyDepleted) {
        return;
    }

    // take energy and put it into burst charge
    drone->burstCharge = fminf(drone->burstCharge + (DRONE_BURST_CHARGE_RATE * e->deltaTime), DRONE_ENERGY_MAX);
    drone->energyLeft = fmaxf(drone->energyLeft - (DRONE_BURST_CHARGE_RATE * e->deltaTime), 0.0f);
    drone->chargingBurst = true;

    if (drone->energyLeft == 0.0f) {
        drone->energyFullyDepleted = true;
        e->stats[drone->idx].energyEmptied++;
    }
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

typedef struct burstExplosionCtx {
    env *e;
    droneEntity *parentDrone;
    const b2ExplosionDef *def;
} burstExplosionCtx;

// b2World_Explode doesn't support filtering on shapes of the same category,
// so we have to do it manually
// mostly copied from box2d/src/world.c
bool burstExplodeCallback(b2ShapeId shapeID, void *context) {
    ASSERT(b2Shape_IsValid(shapeID));

    const burstExplosionCtx *ctx = (burstExplosionCtx *)context;
    const entity *entity = b2Shape_GetUserData(shapeID);
    // the explosion shouldn't affect the parent drone
    if (entity->type == DRONE_ENTITY) {
        droneEntity *drone = (droneEntity *)entity->entity;
        if (drone->idx == ctx->parentDrone->idx) {
            return true;
        }
        ctx->parentDrone->stepInfo.explosionHit[drone->idx] = true;
        ctx->e->stats[ctx->parentDrone->idx].burstsHit++;
        DEBUG_LOGF("drone %d hit drone %d with burst", ctx->parentDrone->idx, drone->idx);
        drone->stepInfo.shotTaken[ctx->parentDrone->idx] = true;
        DEBUG_LOGF("drone %d hit burst from drone %d", drone->idx, ctx->parentDrone->idx);
    }

    b2BodyId bodyID = b2Shape_GetBody(shapeID);
    ASSERT(b2Body_IsValid(bodyID));
    b2Transform transform = b2Body_GetTransform(bodyID);

    bool isCircle = false;
    b2DistanceInput input;
    input.proxyA = makeDistanceProxy(entity, &isCircle);
    input.proxyB = b2MakeProxy(&ctx->def->position, 1, 0.0f);
    input.transformA = transform;
    input.transformB = b2Transform_identity;
    input.useRadii = isCircle;

    b2SimplexCache cache = {0};
    const b2DistanceOutput output = b2ShapeDistance(&cache, &input, NULL, 0);
    if (output.distance > ctx->def->radius + ctx->def->falloff) {
        return true;
    }

    const b2Vec2 closestPoint = output.pointA;
    const b2Vec2 direction = b2Normalize(b2Sub(closestPoint, ctx->def->position));

    b2Vec2 localLine = b2InvRotateVector(transform.q, b2LeftPerp(direction));
    float perimeter = getShapeProjectedPerimeter(shapeID, localLine);
    float scale = 1.0f;
    if (output.distance > ctx->def->radius) {
        scale = clamp((ctx->def->radius + ctx->def->falloff - output.distance) / ctx->def->falloff);
    }

    const float magnitude = (ctx->def->impulsePerLength + b2Length(ctx->parentDrone->lastVelocity)) * perimeter * scale;
    const b2Vec2 impulse = b2MulSV(magnitude, direction);

    b2Body_ApplyLinearImpulseToCenter(bodyID, impulse, true);
    if (entityTypeIsWall(entity->type)) {
        b2Body_ApplyAngularImpulse(bodyID, magnitude, true);
    }

    return true;
}

void burstExplode(env *e, droneEntity *drone, const b2ExplosionDef *def) {
    const float fullRadius = def->radius + def->falloff;
    b2AABB aabb = {
        .lowerBound.x = def->position.x - fullRadius,
        .lowerBound.y = def->position.y - fullRadius,
        .upperBound.x = def->position.x + fullRadius,
        .upperBound.y = def->position.y + fullRadius,
    };

    b2QueryFilter filter = b2DefaultQueryFilter();
    filter.maskBits = def->maskBits;

    burstExplosionCtx ctx = {.e = e, .parentDrone = drone, .def = def};
    b2World_OverlapAABB(e->worldID, aabb, filter, burstExplodeCallback, &ctx);
}

void droneBurst(env *e, droneEntity *drone) {
    if (!drone->chargingBurst) {
        return;
    }

    const b2Vec2 pos = getCachedPos(drone->bodyID, &drone->pos);
    const float radius = (DRONE_BURST_RADIUS_BASE * drone->burstCharge) + DRONE_BURST_RADIUS_MIN;
    b2ExplosionDef explosion = {
        .position = pos,
        .radius = radius,
        .impulsePerLength = (DRONE_BURST_IMPACT_BASE * drone->burstCharge) + DRONE_BURST_IMPACT_MIN,
        .falloff = radius / 2.0f,
        .maskBits = FLOATING_WALL_SHAPE | PROJECTILE_SHAPE | DRONE_SHAPE,
    };
    burstExplode(e, drone, &explosion);

    drone->chargingBurst = false;
    drone->burstCharge = 0.0f;
    if (drone->energyLeft == 0.0f) {
        drone->energyFullyDepletedThisStep = true;
        drone->energyRefillWait = DRONE_ENERGY_REFILL_EMPTY_WAIT * (float)e->frameRate;
    } else {
        drone->energyRefillWait = DRONE_ENERGY_REFILL_WAIT * (float)e->frameRate;
    }
    e->stats[drone->idx].totalBursts++;

    if (e->client != NULL) {
        explosionInfo *explInfo = fastMalloc(sizeof(explosionInfo));
        explInfo->def = explosion;
        explInfo->renderSteps = UINT16_MAX;
        cc_array_add(e->explosions, explInfo);
    }
}

void droneAddEnergy(droneEntity *drone, float energy) {
    // if a burst is charging, add the energy to the burst charge
    if (drone->chargingBurst) {
        drone->burstCharge = clamp(drone->burstCharge + energy);
    } else {
        drone->energyLeft = clamp(drone->energyLeft + energy);
    }
}

void droneDiscardWeapon(env *e, droneEntity *drone) {
    if (drone->weaponInfo->type == e->defaultWeapon->type || (drone->energyFullyDepleted && !drone->chargingBurst)) {
        return;
    }

    droneChangeWeapon(e, drone, e->defaultWeapon->type);
    droneAddEnergy(drone, -WEAPON_PICKUP_ENERGY_REFILL * 2.0f);
    if (drone->chargingBurst) {
        return;
    }

    if (drone->energyLeft == 0.0f) {
        drone->energyFullyDepleted = true;
        drone->energyFullyDepletedThisStep = true;
        drone->energyRefillWait = DRONE_ENERGY_REFILL_EMPTY_WAIT * (float)e->frameRate;
        e->stats[drone->idx].energyEmptied++;
    } else {
        drone->energyRefillWait = DRONE_ENERGY_REFILL_WAIT * (float)e->frameRate;
    }
}

void droneStep(env *e, droneEntity *drone) {
    // manage weapon charge and heat
    drone->weaponCooldown = fmaxf(drone->weaponCooldown - e->deltaTime, 0.0f);
    if (!drone->shotThisStep) {
        drone->charge = fmaxf(drone->charge - 1, 0);
        drone->heat = fmaxf(drone->heat - 1, 0);
    } else {
        drone->shotThisStep = false;
    }
    ASSERT(!drone->shotThisStep);

    // manage drone brake
    if (drone->energyFullyDepletedThisStep) {
        drone->energyFullyDepletedThisStep = false;
    } else if (drone->energyRefillWait != 0.0f) {
        drone->energyRefillWait = fmaxf(drone->energyRefillWait - 1, 0.0f);
    } else if (drone->energyLeft != DRONE_ENERGY_MAX && !drone->chargingBurst) {
        // don't start recharging energy until the burst charge is used
        drone->energyLeft = fminf(drone->energyLeft + (DRONE_ENERGY_REFILL_RATE * e->deltaTime), DRONE_ENERGY_MAX);
    }
    if (drone->energyLeft == DRONE_ENERGY_MAX) {
        drone->energyFullyDepleted = false;
    }

    const b2Vec2 pos = getCachedPos(drone->bodyID, &drone->pos);
    const float distance = b2Distance(drone->lastPos, pos);
    e->stats[drone->idx].distanceTraveled += distance;
    drone->lastPos = pos;

    // update line of sight info for this drone
    for (uint8_t i = 0; i < e->numDrones; i++) {
        if (i == drone->idx || drone->inLineOfSight[i]) {
            continue;
        }

        droneEntity *enemyDrone = safe_array_get_at(e->drones, i);
        const b2Vec2 enemyPos = enemyDrone->pos.pos;
        const float distance = b2Distance(enemyPos, pos);
        const b2Vec2 enemyDirection = b2Normalize(b2Sub(enemyPos, pos));
        const b2Vec2 rayEnd = b2MulAdd(pos, distance, enemyDirection);
        const b2Vec2 translation = b2Sub(rayEnd, pos);
        const b2QueryFilter filter = {.categoryBits = PROJECTILE_SHAPE, .maskBits = WALL_SHAPE | FLOATING_WALL_SHAPE | DRONE_SHAPE};
        const b2RayResult rayRes = b2World_CastRayClosest(e->worldID, pos, translation, filter);

        if (!rayRes.hit) {
            continue;
        }
        ASSERT(b2Shape_IsValid(rayRes.shapeId));
        const entity *ent = b2Shape_GetUserData(rayRes.shapeId);
        if (ent != NULL && ent->type == DRONE_ENTITY) {
            // get the drone entity in case another drone is between the
            // current drone and the enemy drone
            droneEntity *closestDrone = ent->entity;
            closestDrone->inLineOfSight[drone->idx] = true;
            drone->inLineOfSight[closestDrone->idx] = true;
        }
    }
}

void projectilesStep(env *e) {
    CC_SListIter iter;
    cc_slist_iter_init(&iter, e->projectiles);
    projectileEntity *projectile;
    while (cc_slist_iter_next(&iter, (void **)&projectile) != CC_ITER_END) {
        const float maxDistance = projectile->weaponInfo->maxDistance;
        projectile->pos.valid = false;
        const b2Vec2 pos = getCachedPos(projectile->bodyID, &projectile->pos);
        const b2Vec2 distance = b2Sub(pos, projectile->lastPos);
        projectile->distance += b2Length(distance);
        projectile->lastPos = pos;

        if (maxDistance == INFINITE) {
            continue;
        }

        if (projectile->distance >= maxDistance) {
            // we have to destroy the projectile using the iterator so
            // we can continue to iterate correctly, copy the body ID
            // so we can destroy it after the projectile has been freed
            cc_slist_iter_remove(&iter, NULL);
            const b2BodyId bodyID = projectile->bodyID;
            destroyProjectile(e, projectile, false);
            b2DestroyBody(bodyID);
            continue;
        }
    }
}

void weaponPickupsStep(env *e) {
    CC_ArrayIter iter;
    cc_array_iter_init(&iter, e->pickups);
    weaponPickupEntity *pickup;
    while (cc_array_iter_next(&iter, (void **)&pickup) != CC_ITER_END) {
        if (pickup->respawnWait != 0.0f) {
            pickup->respawnWait = fmaxf(pickup->respawnWait - e->deltaTime, 0.0f);
            if (pickup->respawnWait == 0.0f) {
                // b2Vec2 oldPos = pickup->pos;
                b2Vec2 pos;
                if (!findOpenPos(e, WEAPON_PICKUP_SHAPE, &pos)) {
                    const enum cc_stat res = cc_array_iter_remove(&iter, NULL);
                    MAYBE_UNUSED(res);
                    ASSERT(res == CC_OK);
                    DEBUG_LOG("destroying weapon pickup");
                    destroyWeaponPickup(e, pickup, true);
                    continue;
                }
                b2Body_SetTransform(pickup->bodyID, pos, b2Rot_identity);
                pickup->pos = pos;
                pickup->weapon = randWeaponPickupType(e);

                DEBUG_LOGF("respawned weapon pickup at %f, %f", pos.x, pos.y);
                const int16_t cellIdx = entityPosToCellIdx(e, pos);
                if (cellIdx == -1) {
                    ERRORF("invalid position for weapon pickup spawn: (%f, %f)", pos.x, pos.y);
                }
                pickup->mapCellIdx = cellIdx;
                mapCell *cell = safe_array_get_at(e->cells, cellIdx);
                entity *ent = (entity *)b2Shape_GetUserData(pickup->shapeID);
                cell->ent = ent;

                // const bool deleted = kd_delete(e->pickupTree, oldPos.x, oldPos.y);
                // MAYBE_UNUSED(deleted);
                // ASSERT(deleted);
                // kd_insert(e->pickupTree, pickup->pos.x, pickup->pos.y, pickup);
            }
        }
    }
}

// destroy the projectile if it has traveled enough or has bounced enough
// times, and update drone stats if a drone was hit
bool handleProjectileBeginContact(env *e, const entity *proj, const entity *ent) {
    projectileEntity *projectile = (projectileEntity *)proj->entity;
    // e (shape B in the collision) will be NULL if it's another
    // projectile that was just destroyed
    if (ent == NULL || (ent != NULL && ent->type == PROJECTILE_ENTITY)) {
        // always allow projectiles to bounce off each other
        return false;
    } else if (ent->type == BOUNCY_WALL_ENTITY) {
        // always allow projectiles to bounce off bouncy walls
        return false;
    } else if (ent->type != BOUNCY_WALL_ENTITY) {
        projectile->bounces++;
        if (ent->type == DRONE_ENTITY) {
            droneEntity *hitDrone = (droneEntity *)ent->entity;
            if (projectile->droneIdx != hitDrone->idx) {
                droneEntity *shooterDrone = safe_array_get_at(e->drones, projectile->droneIdx);

                droneAddEnergy(shooterDrone, projectile->weaponInfo->energyRefill);
                // add 1 so we can differentiate between no weapon and weapon 0
                shooterDrone->stepInfo.shotHit[hitDrone->idx] = projectile->weaponInfo->type + 1;
                e->stats[shooterDrone->idx].shotsHit[projectile->weaponInfo->type]++;
                DEBUG_LOGF("drone %d hit drone %d with weapon %d", shooterDrone->idx, hitDrone->idx, projectile->weaponInfo->type);
                hitDrone->stepInfo.shotTaken[shooterDrone->idx] = true;
                e->stats[hitDrone->idx].shotsTaken[projectile->weaponInfo->type]++;
                DEBUG_LOGF("drone %d hit by drone %d with weapon %d", hitDrone->idx, shooterDrone->idx, projectile->weaponInfo->type);
            } else {
                hitDrone->stepInfo.ownShotTaken = true;
                e->stats[hitDrone->idx].ownShotsTaken[projectile->weaponInfo->type]++;
                DEBUG_LOGF("drone %d hit by own weapon %d", hitDrone->idx, projectile->weaponInfo->type);
            }
        }
    }
    const uint8_t maxBounces = projectile->weaponInfo->maxBounces;
    if (projectile->bounces == maxBounces) {
        destroyProjectile(e, projectile, true);
        return true;
    }

    return false;
}

void handleProjectileEndContact(env *e, const entity *p) {
    // ensure the projectile's speed doesn't change after bouncing off of something
    projectileEntity *projectile = (projectileEntity *)p->entity;
    b2Vec2 velocity = b2Body_GetLinearVelocity(projectile->bodyID);
    b2Vec2 newVel = b2MulSV(weaponFire(&e->randState, projectile->weaponInfo->type) * projectile->weaponInfo->invMass, b2Normalize(velocity));
    b2Body_SetLinearVelocity(projectile->bodyID, newVel);
}

void handleContactEvents(env *e) {
    b2ContactEvents events = b2World_GetContactEvents(e->worldID);
    for (int i = 0; i < events.beginCount; ++i) {
        const b2ContactBeginTouchEvent *event = events.beginEvents + i;
        entity *e1 = NULL;
        entity *e2 = NULL;

        if (b2Shape_IsValid(event->shapeIdA)) {
            e1 = (entity *)b2Shape_GetUserData(event->shapeIdA);
            ASSERT(e1 != NULL);
        }
        if (b2Shape_IsValid(event->shapeIdB)) {
            e2 = (entity *)b2Shape_GetUserData(event->shapeIdB);
            ASSERT(e2 != NULL);
        }

        if (e1 != NULL) {
            if (e1->type == PROJECTILE_ENTITY) {
                if (handleProjectileBeginContact(e, e1, e2)) {
                    e1 = NULL;
                }
            } else if (e1->type == DEATH_WALL_ENTITY && e2 != NULL && e2->type == DRONE_ENTITY) {
                droneEntity *drone = (droneEntity *)e2->entity;
                drone->dead = true;
            }
        }
        if (e2 != NULL) {
            if (e2->type == PROJECTILE_ENTITY) {
                handleProjectileBeginContact(e, e2, e1);
            } else if (e2->type == DEATH_WALL_ENTITY && e1 != NULL && e1->type == DRONE_ENTITY) {
                droneEntity *drone = (droneEntity *)e1->entity;
                drone->dead = true;
            }
        }
    }

    for (int i = 0; i < events.endCount; ++i) {
        const b2ContactEndTouchEvent *event = events.endEvents + i;
        entity *e1 = NULL;
        entity *e2 = NULL;

        if (b2Shape_IsValid(event->shapeIdA)) {
            e1 = (entity *)b2Shape_GetUserData(event->shapeIdA);
            ASSERT(e1 != NULL);
        }
        if (b2Shape_IsValid(event->shapeIdB)) {
            e2 = (entity *)b2Shape_GetUserData(event->shapeIdB);
            ASSERT(e2 != NULL);
        }

        if (e1 != NULL && e1->type == PROJECTILE_ENTITY) {
            handleProjectileEndContact(e, e1);
        }
        if (e2 != NULL && e2->type == PROJECTILE_ENTITY) {
            handleProjectileEndContact(e, e2);
        }
    }
}

// set pickup to respawn somewhere else randomly if a drone touched it,
// mark the pickup as disabled if a floating wall is touching it
void handleWeaponPickupBeginTouch(env *e, const entity *sensor, entity *visitor) {
    weaponPickupEntity *pickup = (weaponPickupEntity *)sensor->entity;
    if (pickup->respawnWait != 0.0f || pickup->floatingWallsTouching != 0) {
        return;
    }

    switch (visitor->type) {
    case DRONE_ENTITY:
        pickup->respawnWait = PICKUP_RESPAWN_WAIT;
        mapCell *cell = safe_array_get_at(e->cells, pickup->mapCellIdx);
        ASSERT(cell->ent != NULL);
        cell->ent = NULL;

        droneEntity *drone = (droneEntity *)visitor->entity;
        drone->stepInfo.pickedUpWeapon = true;
        drone->stepInfo.prevWeapon = drone->weaponInfo->type;
        droneChangeWeapon(e, drone, pickup->weapon);

        droneAddEnergy(drone, WEAPON_PICKUP_ENERGY_REFILL);

        e->stats[drone->idx].weaponsPickedUp[pickup->weapon]++;
        DEBUG_LOGF("drone %d picked up weapon %d", drone->idx, pickup->weapon);

        break;
    case STANDARD_WALL_ENTITY:
    case BOUNCY_WALL_ENTITY:
    case DEATH_WALL_ENTITY:
        pickup->floatingWallsTouching++;
        break;
    default:
        ERRORF("invalid weapon pickup begin touch visitor %d", visitor->type);
    }
}

// mark the pickup as enabled if no floating walls are touching it
void handleWeaponPickupEndTouch(const entity *sensor, entity *visitor) {
    weaponPickupEntity *pickup = (weaponPickupEntity *)sensor->entity;
    if (pickup->respawnWait != 0.0f) {
        return;
    }

    switch (visitor->type) {
    case DRONE_ENTITY:
        break;
    case STANDARD_WALL_ENTITY:
    case BOUNCY_WALL_ENTITY:
    case DEATH_WALL_ENTITY:
        pickup->floatingWallsTouching--;
        break;
    default:
        ERRORF("invalid weapon pickup end touch visitor %d", visitor->type);
    }
}

void handleSensorEvents(env *e) {
    b2SensorEvents events = b2World_GetSensorEvents(e->worldID);
    for (int i = 0; i < events.beginCount; ++i) {
        const b2SensorBeginTouchEvent *event = events.beginEvents + i;
        if (!b2Shape_IsValid(event->sensorShapeId)) {
            DEBUG_LOG("could not find sensor shape for begin touch event");
            continue;
        }
        entity *s = (entity *)b2Shape_GetUserData(event->sensorShapeId);
        ASSERT(s != NULL);
        ASSERT(s->type == WEAPON_PICKUP_ENTITY);

        if (!b2Shape_IsValid(event->visitorShapeId)) {
            DEBUG_LOG("could not find visitor shape for begin touch event");
            continue;
        }
        entity *v = (entity *)b2Shape_GetUserData(event->visitorShapeId);
        ASSERT(v != NULL);

        handleWeaponPickupBeginTouch(e, s, v);
    }

    for (int i = 0; i < events.endCount; ++i) {
        const b2SensorEndTouchEvent *event = events.endEvents + i;
        if (!b2Shape_IsValid(event->sensorShapeId)) {
            DEBUG_LOG("could not find sensor shape for end touch event");
            continue;
        }
        entity *s = (entity *)b2Shape_GetUserData(event->sensorShapeId);
        ASSERT(s != NULL);
        ASSERT(s->type == WEAPON_PICKUP_ENTITY);

        if (!b2Shape_IsValid(event->visitorShapeId)) {
            DEBUG_LOG("could not find visitor shape for end touch event");
            continue;
        }
        entity *v = (entity *)b2Shape_GetUserData(event->visitorShapeId);
        ASSERT(v != NULL);

        handleWeaponPickupEndTouch(s, v);
    }
}

#endif
