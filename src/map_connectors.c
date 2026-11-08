/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file map_connectors.c
 *     Dungeon layers: connectors between layers, and routing through them.
 * @par Purpose:
 *     Finds the stairs, ladders and elevators on the map, picks which one a
 *     creature should walk to on its way to another layer, and moves it
 *     across when it gets there.
 * @par Comment:
 *     The route between layers is a small graph search over connector ends,
 *     on top of the usual in-layer routing (ariadne). Integer maths and a
 *     fixed search order only, so all players get the same result.
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "map_connectors.h"
#include <string.h>
#include <stdlib.h>
#include "globals.h"
#include "game_legacy.h"
#include "map_layers.h"
#include "map_data.h"
#include "slab_data.h"
#include "room_data.h"
#include "config_terrain.h"
#include "config_trapdoor.h"
#include "config_creature.h"
#include "creature_control.h"
#include "player_data.h"
#include "thing_list.h"
#include "thing_navigate.h"
#include "ariadne.h"
#include "bflib_planar.h"
#include "map_blocks.h"
#include "thing_physics.h"
#include "dungeon_data.h"
#include "map_columns.h"
#include "ariadne_update.h"
#include "thing_data.h"
#include "map_ceiling.h"
#include "config_crtrstates.h"
#include "creature_states.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
static struct LayerConnector layer_connectors[MAX_LAYER_CONNECTORS];
static int layer_connectors_count = 0;
static TbBool layer_connectors_dirty = true;
static GameTurn layer_connectors_built_turn = 0;
/** Goes up each time the list is worked out again, so the stairs shapes know when to follow. */
static unsigned long layer_connectors_generation = 1;
/** The list is rebuilt at least this often, so things the hooks miss (doors placed or sold) show up. */
#define LAYER_CONNECTORS_REFRESH_TURNS 16

/** Route cost of crossing each kind of connector, in map coordinate units (like walking that far). */
static const int32_t layer_connector_cross_cost[] = {0, 3 * COORD_PER_STL * STL_PER_SLB, 4 * COORD_PER_STL * STL_PER_SLB, 2 * COORD_PER_STL * STL_PER_SLB, 2 * COORD_PER_STL * STL_PER_SLB};

/******************************************************************************/

int room_kind_layer_connector_kind(RoomKind rkind)
{
    if (rkind <= 0)
        return LCnK_None;
    if (room_role_matches(rkind, RoRoF_LayerStairs))
        return LCnK_Stairs;
    if (room_role_matches(rkind, RoRoF_LayerElevator))
        return LCnK_Elevator;
    return LCnK_None;
}

void layer_make_stairs_room_available(void)
{
    if (!map_has_layers())
        return;
    for (RoomKind rkind = 1; rkind < game.conf.slab_conf.room_types_count; rkind++)
    {
        if (room_kind_layer_connector_kind(rkind) != LCnK_Stairs)
            continue;
        for (PlayerNumber plyr_idx = 0; plyr_idx < PLAYERS_COUNT; plyr_idx++)
        {
            if (dungeon_invalid(get_dungeon(plyr_idx)))
                continue;
            set_room_available(plyr_idx, rkind, 1, 1);
        }
    }
}

static int slab_room_connector_kind(MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    struct SlabMap *slb = get_slabmap_block(slb_x, slb_y);
    if (slabmap_block_invalid(slb))
        return LCnK_None;
    return room_kind_layer_connector_kind(slab_corresponding_room(slb->kind));
}

/** The ladder (a door with the LADDER property) standing on a slab, or an invalid thing. */
static struct Thing *slab_ladder_thing(MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    struct SlabMap *slb = get_slabmap_block(slb_x, slb_y);
    if (slabmap_block_invalid(slb))
        return INVALID_THING;
    if ((get_slab_stats(slb)->block_flags & SlbAtFlg_IsDoor) == 0)
        return INVALID_THING;
    struct Thing *doortng = get_door_for_position(slab_subtile_center(slb_x), slab_subtile_center(slb_y));
    if (thing_is_invalid(doortng))
        return INVALID_THING;
    if ((get_door_model_stats(doortng->model)->model_flags & DoMF_Ladder) == 0)
        return INVALID_THING;
    return doortng;
}

/** A ladder's top lands on a floor a creature can stand on. */
static TbBool slab_is_ladder_top(MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    struct SlabMap *slb = get_slabmap_block(slb_x, slb_y);
    if (slabmap_block_invalid(slb))
        return false;
    const struct SlabConfigStats *slabst = get_slab_stats(slb);
    if ((slabst->block_flags & (SlbAtFlg_IsDoor|SlbAtFlg_IsRoom)) != 0)
        return ((slabst->block_flags & SlbAtFlg_IsRoom) != 0);
    return ((slabst->block_flags & SlbAtFlg_Blocking) == 0) && !slab_kind_is_liquid(slb->kind);
}

static void dir_delta(int dir, int *dx, int *dy)
{
    static const int ddx[4] = {0, 1, 0, -1};
    static const int ddy[4] = {-1, 0, 1, 0};
    *dx = ddx[dir & 3];
    *dy = ddy[dir & 3];
}

/** Open for stairs: a creature could walk there (a floor, a room, not rock, a wall or a door). */
static TbBool slab_open_for_stairs(MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    struct SlabMap *slb = get_slabmap_block(slb_x, slb_y);
    if (slabmap_block_invalid(slb))
        return false;
    const struct SlabConfigStats *slabst = get_slab_stats(slb);
    if ((slabst->block_flags & SlbAtFlg_IsDoor) != 0)
        return false;
    if ((slabst->block_flags & SlbAtFlg_IsRoom) != 0)
        return true;
    return ((slabst->block_flags & SlbAtFlg_Blocking) == 0);
}

static TbBool slab_is_stairs(MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    return (slab_room_connector_kind(slb_x, slb_y) == LCnK_Stairs);
}

/** Stairs take two stairs tiles in a row on the lower layer: the top one, under the stairs end on the layer
 * above, and the bottom one before it, which has no stairs end above it. Picks the side the stairs rise
 * towards (from the bottom tile to the top one), or -1 if no tile next to the top one can be the bottom.
 * A way off the landing above counts first, then a way in at the bottom; ties go N, E, S, W, so every
 * player gets the same stairs. */
static int pick_stairs_dir(int lower, MapSlabCoord lo_x, MapSlabCoord lo_y, MapSlabCoord up_x, MapSlabCoord up_y)
{
    int best_dir = -1;
    int best_score = -1;
    for (int dir = 0; dir < 4; dir++)
    {
        int dx;
        int dy;
        dir_delta(dir, &dx, &dy);
        const MapSlabCoord bx = lo_x - dx;
        const MapSlabCoord by = lo_y - dy;
        if ((get_layer_at_slab(bx, by) != lower) || !slab_is_stairs(bx, by) || slab_is_stairs(up_x - dx, up_y - dy))
            continue;
        int score = 0;
        if (slab_open_for_stairs(up_x + dx, up_y + dy))
            score += 4;
        if (slab_open_for_stairs(bx - dx, by - dy))
            score += 2;
        if (score > best_score)
        {
            best_score = score;
            best_dir = dir;
        }
    }
    return best_dir;
}

/** A single stairs tile, not (yet) part of stairs: half stairs, going nowhere. */
struct LayerHalfStairs {
    MapSlabCoord slb_x;
    MapSlabCoord slb_y;
    unsigned char dir;
};
#define MAX_LAYER_HALF_STAIRS 64
static struct LayerHalfStairs layer_half_stairs[MAX_LAYER_HALF_STAIRS];
static int layer_half_stairs_count = 0;
static unsigned char layer_stairs_used[MAX_TILES_X * MAX_TILES_Y];

/** Half stairs rise towards another stairs tile next to them if there is one (where the stairs will be
 * finished), else towards open ground. */
static unsigned char pick_half_stairs_dir(MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    const int layer = get_layer_at_slab(slb_x, slb_y);
    for (int pass = 0; pass < 2; pass++)
    {
        for (int dir = 0; dir < 4; dir++)
        {
            int dx;
            int dy;
            dir_delta(dir, &dx, &dy);
            if (get_layer_at_slab(slb_x + dx, slb_y + dy) != layer)
                continue;
            if ((pass == 0) ? slab_is_stairs(slb_x + dx, slb_y + dy) : slab_open_for_stairs(slb_x + dx, slb_y + dy))
                return (unsigned char)dir;
        }
    }
    return LCnD_North;
}

static void find_layer_half_stairs(void)
{
    layer_half_stairs_count = 0;
    for (int layer = 0; layer < game.layers_count; layer++)
    {
        const struct MapLayer *lr = &game.layers[layer];
        for (MapSlabCoord slb_y = lr->y0; slb_y <= lr->y1; slb_y++)
        {
            for (MapSlabCoord slb_x = lr->x0; slb_x <= lr->x1; slb_x++)
            {
                if (layer_stairs_used[get_slab_number(slb_x, slb_y)] || !slab_is_stairs(slb_x, slb_y))
                    continue;
                if (layer_half_stairs_count >= MAX_LAYER_HALF_STAIRS)
                    return;
                struct LayerHalfStairs *half = &layer_half_stairs[layer_half_stairs_count++];
                half->slb_x = slb_x;
                half->slb_y = slb_y;
                half->dir = pick_half_stairs_dir(slb_x, slb_y);
            }
        }
    }
}

/** Pits for fliers: each group of touching pit tiles on a layer becomes one connector, at its first tile
 * in map order (top row first), so the router stays small and every player gets the same list. */
static unsigned char layer_pit_seen[MAX_TILES_X * MAX_TILES_Y];
static MapSlabCoord layer_pit_stack_x[MAX_TILES_X * MAX_TILES_Y];
static MapSlabCoord layer_pit_stack_y[MAX_TILES_X * MAX_TILES_Y];

static void add_layer_pit_connectors(void)
{
    memset(layer_pit_seen, 0, sizeof(layer_pit_seen));
    for (int upper = 1; upper < game.layers_count; upper++)
    {
        const struct MapLayer *up = &game.layers[upper];
        const struct MapLayer *lo = &game.layers[upper - 1];
        for (MapSlabCoord slb_y = up->y0; slb_y <= up->y1; slb_y++)
        {
            for (MapSlabCoord slb_x = up->x0; slb_x <= up->x1; slb_x++)
            {
                if (layer_pit_seen[get_slab_number(slb_x, slb_y)] || !slab_is_layer_fall_pit(slb_x, slb_y))
                    continue;
                // Mark the whole group, so it gives a single connector.
                int top = 0;
                layer_pit_seen[get_slab_number(slb_x, slb_y)] = 1;
                layer_pit_stack_x[top] = slb_x;
                layer_pit_stack_y[top] = slb_y;
                top++;
                while (top > 0)
                {
                    top--;
                    const MapSlabCoord cx = layer_pit_stack_x[top];
                    const MapSlabCoord cy = layer_pit_stack_y[top];
                    for (int i = 0; i < SMALL_AROUND_LENGTH; i++)
                    {
                        const MapSlabCoord nx = cx + small_around[i].delta_x;
                        const MapSlabCoord ny = cy + small_around[i].delta_y;
                        if ((nx < up->x0) || (nx > up->x1) || (ny < up->y0) || (ny > up->y1))
                            continue;
                        if (layer_pit_seen[get_slab_number(nx, ny)] || !slab_is_layer_fall_pit(nx, ny))
                            continue;
                        layer_pit_seen[get_slab_number(nx, ny)] = 1;
                        layer_pit_stack_x[top] = nx;
                        layer_pit_stack_y[top] = ny;
                        top++;
                    }
                }
                if (layer_connectors_count >= MAX_LAYER_CONNECTORS)
                {
                    WARNLOG("Too many layer connectors, only %d are used", MAX_LAYER_CONNECTORS);
                    return;
                }
                struct LayerConnector *conn = &layer_connectors[layer_connectors_count++];
                conn->kind = LCnK_Pit;
                conn->lower_layer = upper - 1;
                conn->owner = game.neutral_player_num;
                conn->lower_slb_x = slb_x + lo->x0 - up->x0;
                conn->lower_slb_y = slb_y + lo->y0 - up->y0;
                conn->upper_slb_x = slb_x;
                conn->upper_slb_y = slb_y;
                conn->dir = LCnD_North;
            }
        }
    }
}

static void rebuild_layer_connectors(void)
{
    layer_connectors_count = 0;
    layer_connectors_dirty = false;
    memset(layer_stairs_used, 0, sizeof(layer_stairs_used));
    layer_connectors_generation++;
    layer_connectors_built_turn = game.play_gameturn;
    for (int lower = 0; lower + 1 < game.layers_count; lower++)
    {
        const struct MapLayer *lo = &game.layers[lower];
        const struct MapLayer *up = &game.layers[lower + 1];
        const int width = min(lo->x1 - lo->x0, up->x1 - up->x0) + 1;
        const int height = min(lo->y1 - lo->y0, up->y1 - up->y0) + 1;
        for (int ly = 0; ly < height; ly++)
        {
            for (int lx = 0; lx < width; lx++)
            {
                const MapSlabCoord lo_x = lo->x0 + lx;
                const MapSlabCoord lo_y = lo->y0 + ly;
                const MapSlabCoord up_x = up->x0 + lx;
                const MapSlabCoord up_y = up->y0 + ly;
                int kind = LCnK_None;
                int dir = LCnD_North;
                PlayerNumber owner = game.neutral_player_num;
                const int room_kind = slab_room_connector_kind(lo_x, lo_y);
                if ((room_kind != LCnK_None) && (slab_room_connector_kind(up_x, up_y) == room_kind))
                {
                    kind = room_kind;
                    owner = slabmap_owner(get_slabmap_block(lo_x, lo_y));
                    if (kind == LCnK_Stairs)
                    {
                        dir = pick_stairs_dir(lower, lo_x, lo_y, up_x, up_y);
                        if (dir < 0)
                            kind = LCnK_None;
                    }
                } else
                {
                    struct Thing *laddertng = slab_ladder_thing(lo_x, lo_y);
                    if (!thing_is_invalid(laddertng) && slab_is_ladder_top(up_x, up_y))
                    {
                        kind = LCnK_Ladder;
                        owner = laddertng->owner;
                    }
                }
                if (kind == LCnK_None)
                    continue;
                if (layer_connectors_count >= MAX_LAYER_CONNECTORS)
                {
                    WARNLOG("Too many layer connectors, only %d are used", MAX_LAYER_CONNECTORS);
                    return;
                }
                struct LayerConnector *conn = &layer_connectors[layer_connectors_count++];
                conn->kind = kind;
                conn->lower_layer = lower;
                conn->owner = owner;
                conn->lower_slb_x = lo_x;
                conn->lower_slb_y = lo_y;
                conn->upper_slb_x = up_x;
                conn->upper_slb_y = up_y;
                conn->dir = (unsigned char)dir;
                if (kind == LCnK_Stairs)
                {
                    int dx;
                    int dy;
                    dir_delta(dir, &dx, &dy);
                    layer_stairs_used[get_slab_number(lo_x, lo_y)] = 1;
                    layer_stairs_used[get_slab_number(lo_x - dx, lo_y - dy)] = 1;
                    layer_stairs_used[get_slab_number(up_x, up_y)] = 1;
                }
            }
        }
    }
    find_layer_half_stairs();
    add_layer_pit_connectors();
    SYNCDBG(8, "Found %d layer connectors", layer_connectors_count);
}

static void ensure_layer_connectors(void)
{
    if (game.layers_count < 2)
    {
        layer_connectors_count = 0;
        return;
    }
    // The refresh windows are fixed turns, so the list is the same whoever asks first.
    if (layer_connectors_dirty
        || (game.play_gameturn / LAYER_CONNECTORS_REFRESH_TURNS != layer_connectors_built_turn / LAYER_CONNECTORS_REFRESH_TURNS))
    {
        rebuild_layer_connectors();
    }
}

void layer_connectors_changed(void)
{
    layer_connectors_dirty = true;
}

int get_layer_connectors_count(void)
{
    ensure_layer_connectors();
    return layer_connectors_count;
}

const struct LayerConnector *get_layer_connector(int idx)
{
    ensure_layer_connectors();
    if ((idx < 0) || (idx >= layer_connectors_count))
        return NULL;
    return &layer_connectors[idx];
}

int get_layer_connector_at_slab(MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    ensure_layer_connectors();
    for (int i = 0; i < layer_connectors_count; i++)
    {
        const struct LayerConnector *conn = &layer_connectors[i];
        if (((conn->lower_slb_x == slb_x) && (conn->lower_slb_y == slb_y))
         || ((conn->upper_slb_x == slb_x) && (conn->upper_slb_y == slb_y)))
            return i;
    }
    return -1;
}

TbBool layer_connector_usable_by(const struct LayerConnector *conn, const struct Thing *creatng)
{
    if (conn == NULL)
        return false;
    const unsigned long mflags = get_creature_model_flags(creatng);
    switch (conn->kind)
    {
    case LCnK_Stairs:
        if ((mflags & CMF_LayerNoStairs) != 0)
            return false;
        break;
    case LCnK_Elevator:
        if ((mflags & CMF_LayerNoElevator) != 0)
            return false;
        break;
    case LCnK_Pit:
        if (!flag_is_set(creatng->movement_flags, TMvF_Flying))
            return false;
        break;
    case LCnK_Ladder:
    {
        if ((mflags & CMF_LayerLadders) == 0)
            return false;
        // A locked ladder is only for its owner and allies, like a locked door.
        struct Thing *laddertng = slab_ladder_thing(conn->lower_slb_x, conn->lower_slb_y);
        if (thing_is_invalid(laddertng))
            return false;
        if (laddertng->door.is_locked && players_are_enemies(creatng->owner, laddertng->owner))
            return false;
        break;
    }
    default:
        return false;
    }
    return true;
}

/******************************************************************************/

/* Stairs: three steps rising along 'r' (r = 0 is the side the stairs are entered from), 'a' is across.
 * Walkers don't climb the steps one by one: they follow a straight ramp from the room floor at the
 * entry edge up to the floor of the layer above at the far end of the top tile, which is where the
 * climb to the layer above starts. The steps are drawn just under the ramp. */
#define STAIRS_RAMP_END (2 * STL_PER_SLB * COORD_PER_STL)
/** How much the ramp rises, in cubes: a storey, so it ends level with the floor of the layer above. */
#define STAIRS_RISE_CUBES LAYER_STOREY_CUBES

/** Stairs coordinates of a position inside the slab, given as offsets 0..size-1 from its corner. */
static void stairs_local(int dir, int lx, int ly, int size, int *a, int *r)
{
    switch (dir)
    {
    case LCnD_North: *a = lx;              *r = (size - 1) - ly; break;
    case LCnD_East:  *a = ly;              *r = lx;              break;
    case LCnD_South: *a = (size - 1) - lx; *r = ly;              break;
    default:         *a = (size - 1) - ly; *r = (size - 1) - lx; break;
    }
}

/** Row of a subtile on stairs, from the bottom step (0) to the top step (2). */
static int stairs_row(int dir, MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    int a;
    int r;
    stairs_local(dir, stl_x % STL_PER_SLB, stl_y % STL_PER_SLB, STL_PER_SLB, &a, &r);
    return r;
}

/** Height of the ramp at a distance along the rise (map coords from the entry edge). */
static MapCoord stairs_ramp_height(int base_h, MapCoord along)
{
    along = max((MapCoord)0, min(along, (MapCoord)STAIRS_RAMP_END));
    return (MapCoord)base_h * COORD_PER_STL + (MapCoord)STAIRS_RISE_CUBES * COORD_PER_STL * along / STAIRS_RAMP_END;
}

/** Where a creature starts its climb on one end: the top step and the landing for stairs, the middle otherwise. */
static void connector_climb_subtile(const struct LayerConnector *conn, TbBool upper, MapSubtlCoord *stl_x, MapSubtlCoord *stl_y)
{
    const MapSlabCoord slb_x = upper ? conn->upper_slb_x : conn->lower_slb_x;
    const MapSlabCoord slb_y = upper ? conn->upper_slb_y : conn->lower_slb_y;
    *stl_x = slab_subtile_center(slb_x);
    *stl_y = slab_subtile_center(slb_y);
    if (conn->kind == LCnK_Stairs)
    {
        int dx;
        int dy;
        dir_delta(conn->dir, &dx, &dy);
        *stl_x += dx;
        *stl_y += dy;
    }
}

static void connector_climb_pos(const struct LayerConnector *conn, TbBool upper, struct Coord3d *pos)
{
    MapSubtlCoord stl_x;
    MapSubtlCoord stl_y;
    connector_climb_subtile(conn, upper, &stl_x, &stl_y);
    pos->x.val = subtile_coord_center(stl_x);
    pos->y.val = subtile_coord_center(stl_y);
    pos->z.val = get_floor_height(stl_x, stl_y);
}

/** True if the subtile is where a climb starts on that end of the connector. */
static TbBool subtile_is_connector_climb_spot(const struct LayerConnector *conn, TbBool upper, MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if (conn->kind == LCnK_Stairs)
        return (stairs_row(conn->dir, stl_x, stl_y) == STL_PER_SLB - 1);
    MapSubtlCoord cx;
    MapSubtlCoord cy;
    connector_climb_subtile(conn, upper, &cx, &cy);
    return (stl_x == cx) && (stl_y == cy);
}

static TbBool layer_connector_placing = false;

void layer_connector_room_placed(PlayerNumber owner, RoomKind rkind, MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    layer_connectors_changed();
    if (layer_connector_placing || !map_has_layers())
        return;
    if (room_kind_layer_connector_kind(rkind) == LCnK_None)
        return;
    const int layer = get_layer_at_slab(slb_x, slb_y);
    if (layer < 0)
        return;
    int targets_count = 2;
    if (room_kind_layer_connector_kind(rkind) == LCnK_Stairs)
    {
        // Stairs take two tiles: a single tile is half stairs going nowhere, and the tile built next to
        // half stairs finishes them, so it gets the stairs end on the layer above.
        ensure_layer_connectors();
        if (layer_stairs_used[get_slab_number(slb_x, slb_y)])
            return;
        TbBool next_to_half_stairs = false;
        for (int dir = 0; dir < 4; dir++)
        {
            int dx;
            int dy;
            dir_delta(dir, &dx, &dy);
            const MapSlabCoord bx = slb_x - dx;
            const MapSlabCoord by = slb_y - dy;
            if ((get_layer_at_slab(bx, by) == layer) && slab_is_stairs(bx, by) && !layer_stairs_used[get_slab_number(bx, by)])
                next_to_half_stairs = true;
        }
        if (!next_to_half_stairs)
            return;
        targets_count = 1;
    }
    // The matching end goes on the layer above, or on the layer below if the one above can't take it.
    const int targets[2] = {layer + 1, layer - 1};
    for (int t = 0; t < targets_count; t++)
    {
        const int other = targets[t];
        if ((other < 0) || (other >= game.layers_count))
            continue;
        const MapSlabCoord ox = slb_x + game.layers[other].x0 - game.layers[layer].x0;
        const MapSlabCoord oy = slb_y + game.layers[other].y0 - game.layers[layer].y0;
        struct SlabMap *slb = get_slabmap_block(ox, oy);
        if (slabmap_block_invalid(slb))
            continue;
        if (slab_corresponding_room(slb->kind) == rkind)
            return; // Already connected
        if (slb->room_index > 0)
            continue;
        // On the player's own floor, or on an unclaimed path, which is claimed for it: at first the
        // player's imps can't reach another layer to claim anything there.
        if (!((slb->kind == SlbT_CLAIMED) && (slabmap_owner(slb) == owner)) && (slb->kind != SlbT_PATH))
            continue;
        if (slab_has_trap_on(ox, oy) || slab_has_door_thing_on(ox, oy))
            continue;
        layer_connector_placing = true;
        if (slb->kind == SlbT_PATH)
        {
            place_slab_type_on_map(SlbT_CLAIMED, slab_subtile_center(ox), slab_subtile_center(oy), owner, 1);
            increase_dungeon_area(owner, 1);
        }
        place_room(owner, rkind, slab_subtile(ox, 0), slab_subtile(oy, 0));
        layer_connector_placing = false;
        SYNCDBG(7, "Placed the %s end on layer %d at slab (%d,%d)", room_code_name(rkind), other, (int)ox, (int)oy);
        return;
    }
}

void layer_connector_room_owner_changed(struct Room *room)
{
    static TbBool updating = false;
    if (updating || !map_has_layers() || room_is_invalid(room))
        return;
    if (room_kind_layer_connector_kind(room->kind) == LCnK_None)
        return;
    updating = true;
    // Both ends of stairs belong to the same player: the upper end can't be claimed by walking onto it
    // (it is mostly a stairwell), and owning it lets imps claim the ground around it on that layer.
    // The slabs are listed first, as taking over a room may merge it with others.
    SlabCodedCoords slbs[MAX_TILES_X];
    int count = 0;
    for (SlabCodedCoords i = room->slabs_list; (i != 0) && (count < MAX_TILES_X); i = get_next_slab_number_in_room(i))
        slbs[count++] = i;
    const PlayerNumber owner = room->owner;
    const RoomKind rkind = room->kind;
    for (int k = 0; k < count; k++)
    {
        const MapSlabCoord slb_x = slb_num_decode_x(slbs[k]);
        const MapSlabCoord slb_y = slb_num_decode_y(slbs[k]);
        const int layer = get_layer_at_slab(slb_x, slb_y);
        if (layer < 0)
            continue;
        for (int other = layer - 1; other <= layer + 1; other += 2)
        {
            if ((other < 0) || (other >= game.layers_count))
                continue;
            const MapSlabCoord ox = slb_x + game.layers[other].x0 - game.layers[layer].x0;
            const MapSlabCoord oy = slb_y + game.layers[other].y0 - game.layers[layer].y0;
            struct Room *oroom = slab_room_get(ox, oy);
            if (room_exists(oroom) && (oroom->kind == rkind) && (oroom->owner != owner))
            {
                SYNCDBG(7, "The %s end on layer %d at slab (%d,%d) follows to player %d", room_code_name(rkind), other, (int)ox, (int)oy, (int)owner);
                take_over_room(oroom, owner);
            }
        }
    }
    layer_connectors_changed();
    updating = false;
}

/******************************************************************************/
// Route between layers: a shortest-path search over connector ends.

#define LAYER_ROUTE_MAX_NODES (2 + 2 * MAX_LAYER_CONNECTORS)
#define LAYER_ROUTE_NO_DIST INT32_MAX

struct LayerRouteNode {
    struct Coord3d pos;
    int layer;
    int connector; // -1 for the start and the goal
    int32_t dist;
    int prev;
    TbBool done;
};

static struct LayerRouteNode route_nodes[LAYER_ROUTE_MAX_NODES];

TbBool creature_target_on_other_layer(const struct Thing *creatng, const struct Coord3d *pos)
{
    if (!map_has_layers())
        return false;
    const int from = get_layer_at_coord(creatng->mappos.x.val, creatng->mappos.y.val);
    const int to = get_layer_at_coord(pos->x.val, pos->y.val);
    return (from >= 0) && (to >= 0) && (from != to);
}

TbBool layer_route_next_hop(const struct Thing *creatng, const struct Coord3d *target, struct Coord3d *hop)
{
    if (!creature_target_on_other_layer(creatng, target))
        return false;
    ensure_layer_connectors();
    // Nodes: 0 is the start, 1 the goal, then both ends of each connector the creature can use.
    int count = 0;
    struct LayerRouteNode *node = &route_nodes[count++];
    node->pos = creatng->mappos;
    node->layer = get_layer_at_coord(creatng->mappos.x.val, creatng->mappos.y.val);
    node->connector = -1;
    node = &route_nodes[count++];
    node->pos = *target;
    node->layer = get_layer_at_coord(target->x.val, target->y.val);
    node->connector = -1;
    for (int i = 0; i < layer_connectors_count; i++)
    {
        const struct LayerConnector *conn = &layer_connectors[i];
        if (!layer_connector_usable_by(conn, creatng))
            continue;
        node = &route_nodes[count++];
        connector_climb_pos(conn, false, &node->pos);
        node->layer = conn->lower_layer;
        node->connector = i;
        node = &route_nodes[count++];
        connector_climb_pos(conn, true, &node->pos);
        node->layer = conn->lower_layer + 1;
        node->connector = i;
    }
    for (int n = 0; n < count; n++)
    {
        route_nodes[n].dist = LAYER_ROUTE_NO_DIST;
        route_nodes[n].prev = -1;
        route_nodes[n].done = false;
    }
    route_nodes[0].dist = 0;
    struct Coord3d dsts[LAYER_ROUTE_MAX_NODES];
    int dst_nodes[LAYER_ROUTE_MAX_NODES];
    TbBool reachable[LAYER_ROUTE_MAX_NODES];
    while (true)
    {
        // Closest node not done yet; ties go to the lowest index, so the result is the same everywhere.
        int u = -1;
        for (int n = 0; n < count; n++)
        {
            if (!route_nodes[n].done && (route_nodes[n].dist != LAYER_ROUTE_NO_DIST)
                && ((u < 0) || (route_nodes[n].dist < route_nodes[u].dist)))
                u = n;
        }
        if ((u < 0) || (u == 1))
            break;
        struct LayerRouteNode *un = &route_nodes[u];
        un->done = true;
        // Walking to other nodes on the same layer
        int dsts_count = 0;
        for (int n = 1; n < count; n++)
        {
            if ((n == u) || route_nodes[n].done || (route_nodes[n].layer != un->layer))
                continue;
            dsts[dsts_count] = route_nodes[n].pos;
            dst_nodes[dsts_count] = n;
            dsts_count++;
        }
        if (dsts_count > 0)
        {
            ariadne_reachable_from(creatng, &un->pos, dsts, dsts_count, reachable);
            for (int d = 0; d < dsts_count; d++)
            {
                if (!reachable[d])
                    continue;
                struct LayerRouteNode *vn = &route_nodes[dst_nodes[d]];
                const int32_t dist = un->dist + (int32_t)get_chessboard_distance(&un->pos, &vn->pos);
                if (dist < vn->dist)
                {
                    vn->dist = dist;
                    vn->prev = u;
                }
            }
        }
        // Crossing the connector to its other end
        if (un->connector >= 0)
        {
            const int v = ((u - 2) ^ 1) + 2;
            struct LayerRouteNode *vn = &route_nodes[v];
            const int32_t dist = un->dist + layer_connector_cross_cost[layer_connectors[un->connector].kind];
            if (!vn->done && (dist < vn->dist))
            {
                vn->dist = dist;
                vn->prev = u;
            }
        }
    }
    if (route_nodes[1].dist == LAYER_ROUTE_NO_DIST)
    {
        SYNCDBG(9, "No route between layers for %s index %d", thing_model_name(creatng), (int)creatng->index);
        return false;
    }
    // Walk back from the goal to the first node after the start
    int n = 1;
    while ((route_nodes[n].prev > 0) && (route_nodes[n].prev != 0))
        n = route_nodes[n].prev;
    if (route_nodes[n].connector < 0)
        return false;
    *hop = route_nodes[n].pos;
    return true;
}

/** Which connector ends can be walked to from a place, through any number of stairs or elevators.
 * Worked out once per turn for a place, as wander points are checked many at a time against the same heart. */
static struct {
    GameTurn turn;
    MapCoord from_x;
    MapCoord from_y;
    int count;
    TbBool valid;
    TbBool reached[MAX_LAYER_CONNECTORS][2];
} layer_reach_cache;

static TbBool connector_walkable_between_layers(const struct LayerConnector *conn)
{
    // Pits are only for fliers; the wander points are for every creature of a player.
    return (conn->kind == LCnK_Stairs) || (conn->kind == LCnK_Elevator) || (conn->kind == LCnK_Ladder);
}

static void refresh_layer_reach(const struct Coord3d *from)
{
    ensure_layer_connectors();
    if (layer_reach_cache.valid && (layer_reach_cache.turn == game.play_gameturn)
        && (layer_reach_cache.from_x == from->x.val) && (layer_reach_cache.from_y == from->y.val)
        && (layer_reach_cache.count == layer_connectors_count))
        return;
    layer_reach_cache.turn = game.play_gameturn;
    layer_reach_cache.from_x = from->x.val;
    layer_reach_cache.from_y = from->y.val;
    layer_reach_cache.count = layer_connectors_count;
    layer_reach_cache.valid = true;
    memset(layer_reach_cache.reached, 0, sizeof(layer_reach_cache.reached));
    struct Coord3d ends[MAX_LAYER_CONNECTORS][2];
    int queue[2 * MAX_LAYER_CONNECTORS];
    int qhead = 0;
    int qtail = 0;
    struct Coord3d start = *from;
    for (int i = 0; i < layer_connectors_count; i++)
    {
        for (int side = 0; side < 2; side++)
        {
            connector_climb_pos(&layer_connectors[i], side, &ends[i][side]);
            if (connector_walkable_between_layers(&layer_connectors[i]) && navigation_points_connected(&start, &ends[i][side]))
            {
                layer_reach_cache.reached[i][side] = true;
                queue[qtail++] = 2 * i + side;
            }
        }
    }
    // Each end reached is queued once; from it, the connector can be crossed, or other ends walked to.
    while (qhead < qtail)
    {
        const int e = queue[qhead++];
        const int i = e / 2;
        const int side_e = e % 2;
        if (!layer_reach_cache.reached[i][side_e ^ 1])
        {
            layer_reach_cache.reached[i][side_e ^ 1] = true;
            queue[qtail++] = 2 * i + (side_e ^ 1);
        }
        for (int j = 0; j < layer_connectors_count; j++)
        {
            if (!connector_walkable_between_layers(&layer_connectors[j]))
                continue;
            for (int side = 0; side < 2; side++)
            {
                if (layer_reach_cache.reached[j][side])
                    continue;
                if (navigation_points_connected(&ends[i][side_e], &ends[j][side]))
                {
                    layer_reach_cache.reached[j][side] = true;
                    queue[qtail++] = 2 * j + side;
                }
            }
        }
    }
}

TbBool layer_points_connected(const struct Coord3d *from, const struct Coord3d *to)
{
    struct Coord3d a = *from;
    struct Coord3d b = *to;
    if (navigation_points_connected(&a, &b))
        return true;
    if (!map_has_layers())
        return false;
    refresh_layer_reach(from);
    for (int i = 0; i < layer_reach_cache.count; i++)
    {
        for (int side = 0; side < 2; side++)
        {
            if (!layer_reach_cache.reached[i][side])
                continue;
            struct Coord3d end;
            connector_climb_pos(&layer_connectors[i], side, &end);
            if (navigation_points_connected(&end, &b))
                return true;
        }
    }
    return false;
}

TbBool layer_stairs_foot_under(const struct Thing *creatng, struct Coord3d *foot)
{
    if (!map_has_layers())
        return false;
    ensure_layer_connectors();
    const MapSlabCoord slb_x = coord_slab(creatng->mappos.x.val);
    const MapSlabCoord slb_y = coord_slab(creatng->mappos.y.val);
    for (int i = 0; i < layer_connectors_count; i++)
    {
        const struct LayerConnector *conn = &layer_connectors[i];
        if (conn->kind != LCnK_Stairs)
            continue;
        int dx;
        int dy;
        dir_delta(conn->dir, &dx, &dy);
        const TbBool on_top = (slb_x == conn->lower_slb_x) && (slb_y == conn->lower_slb_y);
        const TbBool on_bottom = (slb_x == conn->lower_slb_x - dx) && (slb_y == conn->lower_slb_y - dy);
        if (!on_top && !on_bottom)
            continue;
        const MapSlabCoord fx = conn->lower_slb_x - 2 * dx;
        const MapSlabCoord fy = conn->lower_slb_y - 2 * dy;
        foot->x.val = subtile_coord_center(slab_subtile_center(fx));
        foot->y.val = subtile_coord_center(slab_subtile_center(fy));
        foot->z.val = get_floor_height(slab_subtile_center(fx), slab_subtile_center(fy));
        return true;
    }
    return false;
}

TbBool layer_route_entry_towards(const struct Thing *creatng, const struct Coord3d *target, struct Coord3d *entry)
{
    if (!creature_target_on_other_layer(creatng, target))
        return false;
    ensure_layer_connectors();
    const int to_layer = get_layer_at_coord(target->x.val, target->y.val);
    TbBool found = false;
    MapCoordDelta best_dist = 0;
    // Ties go to the lowest connector index, so every player picks the same one.
    for (int i = 0; i < layer_connectors_count; i++)
    {
        const struct LayerConnector *conn = &layer_connectors[i];
        TbBool upper;
        if (conn->lower_layer == to_layer)
            upper = false;
        else if (conn->lower_layer + 1 == to_layer)
            upper = true;
        else
            continue;
        if (!layer_connector_usable_by(conn, creatng))
            continue;
        struct Coord3d pos;
        connector_climb_pos(conn, upper, &pos);
        if ((conn->kind == LCnK_Stairs) && !upper)
        {
            // At the foot of the stairs: the tile before the bottom one.
            int dx;
            int dy;
            dir_delta(conn->dir, &dx, &dy);
            const MapSlabCoord fx = conn->lower_slb_x - 2 * dx;
            const MapSlabCoord fy = conn->lower_slb_y - 2 * dy;
            pos.x.val = subtile_coord_center(slab_subtile_center(fx));
            pos.y.val = subtile_coord_center(slab_subtile_center(fy));
            pos.z.val = get_floor_height(slab_subtile_center(fx), slab_subtile_center(fy));
        }
        const MapCoordDelta dist = get_chessboard_distance(&pos, target);
        if (found && (dist >= best_dist))
            continue;
        struct Coord3d hop;
        if (!layer_route_next_hop(creatng, &pos, &hop))
            continue;
        *entry = pos;
        best_dist = dist;
        found = true;
    }
    return found;
}

TbBool layer_route_hop_still_valid(const struct Thing *creatng, const struct Coord3d *hop)
{
    if ((hop->x.val == 0) && (hop->y.val == 0))
        return false;
    const int layer = get_layer_at_coord(creatng->mappos.x.val, creatng->mappos.y.val);
    if (get_layer_at_coord(hop->x.val, hop->y.val) != layer)
        return false;
    const int idx = get_layer_connector_at_slab(coord_slab(hop->x.val), coord_slab(hop->y.val));
    if (idx < 0)
        return false;
    return layer_connector_usable_by(get_layer_connector(idx), creatng);
}

/******************************************************************************/
// Stairs shapes: steps on the lower end, and an open well above the steps on the upper end.
// They are worked out from the connector list, never saved, and read by get_map_column().

#define LAYER_STL_HOLE 0xFFFF
/** On the lower layer, the row of subtiles just past the end of the top step: where the floor of the layer
 * above begins. Walkers on the steps can't go on there; they cross to the layer above instead. */
#define LAYER_STL_BEYOND 0xFFFE

struct LayerShapedSlab {
    MapSlabCoord slb_x;
    MapSlabCoord slb_y;
    unsigned char dir;
    unsigned char upper;
};

static unsigned short layer_stl_shape[MAX_SUBTILES_X * MAX_SUBTILES_Y];
#define LAYER_STEP_COLUMNS_MAX ((3 * MAX_LAYER_CONNECTORS + MAX_LAYER_HALF_STAIRS) * STL_PER_SLB * STL_PER_SLB)
#define LAYER_SHAPED_SLABS_MAX (3 * MAX_LAYER_CONNECTORS + MAX_LAYER_HALF_STAIRS)
static struct Column layer_step_columns[LAYER_STEP_COLUMNS_MAX];
/** For each step column: the stairs it belongs to, so walking on it can follow a smooth slope. */
struct LayerStepInfo {
    MapSlabCoord slb_x;
    MapSlabCoord slb_y;
    unsigned char dir;
    unsigned char base_h; /**< Floor height of the room under the steps, in cubes. */
    unsigned char row;    /**< 0 for the first step on the bottom tile, 5 for the last one on the top tile. */
    unsigned char landing; /**< On the upper end: the step one cube under that layer's floor, next to the stairwell. */
};
static struct LayerStepInfo layer_step_info[LAYER_STEP_COLUMNS_MAX];
static struct LayerShapedSlab layer_shaped_slabs[LAYER_SHAPED_SLABS_MAX];
static int layer_shaped_slabs_count = 0;
static unsigned long layer_shapes_generation = 0;
/** Set when the shapes were worked out without updating navigation (loading): the first turn does it. */
static TbBool layer_shapes_nav_pending = false;

/** Height of a step, in cubes: level with the ramp at the middle of the step, as far as columns go; the
 * last steps can't be as tall as the ramp, which goes on to the floor of the layer above. */
static int stairs_step_height(int base_h, int row)
{
    const int h = (int)((stairs_ramp_height(base_h, row * COORD_PER_STL + COORD_PER_STL / 2) + COORD_PER_STL / 2) / COORD_PER_STL);
    return min(h, COLUMN_STACK_HEIGHT);
}

static void make_step_column(struct Column *col, const struct Column *base, int row)
{
    *col = *base;
    int base_h = get_column_floor_filled_subtiles(base);
    if (base_h < 1)
        base_h = 1;
    const int h = stairs_step_height(base_h, row);
    // Every step is a stack of the floor's own top cube, so its top looks like the room floor.
    unsigned short cube = base->cubes[base_h - 1];
    if (cube == 0)
        cube = base->cubes[0];
    if (cube == 0)
        cube = 1;
    for (int i = 0; i < COLUMN_STACK_HEIGHT; i++)
        col->cubes[i] = (i < h) ? cube : 0;
    col->bitfields &= ~CLF_CEILING_MASK;
    set_column_floor_filled_subtiles(col, h);
    make_solidmask(col);
}

/** The landing step on the upper end: no cubes at all, so its floor is one cube under that layer's
 * floor, which is the height between the top step below and that floor. */
static void make_landing_column(struct Column *col, const struct Column *base)
{
    *col = *base;
    for (int i = 0; i < COLUMN_STACK_HEIGHT; i++)
        col->cubes[i] = 0;
    col->bitfields &= ~CLF_CEILING_MASK;
    set_column_floor_filled_subtiles(col, 0);
    make_solidmask(col);
}

/** Puts the landing step on the upper end of stairs: the row next to the stairwell, over the top step. */
static void shape_stairs_landing(const struct LayerConnector *conn, int *col_count)
{
    for (int sy = 0; sy < STL_PER_SLB; sy++)
    {
        for (int sx = 0; sx < STL_PER_SLB; sx++)
        {
            const MapSubtlCoord stl_x = slab_subtile(conn->upper_slb_x, sx);
            const MapSubtlCoord stl_y = slab_subtile(conn->upper_slb_y, sy);
            if (stairs_row(conn->dir, stl_x, stl_y) != STL_PER_SLB - 1)
                continue;
            if (*col_count >= LAYER_STEP_COLUMNS_MAX)
                return;
            struct Map *mapblk = get_map_block_at(stl_x, stl_y);
            make_landing_column(&layer_step_columns[*col_count], &game.columns_data[get_mapblk_column_index(mapblk)]);
            struct LayerStepInfo *info = &layer_step_info[*col_count];
            info->slb_x = conn->upper_slb_x;
            info->slb_y = conn->upper_slb_y;
            info->dir = conn->dir;
            info->base_h = 0;
            info->row = STL_PER_SLB - 1;
            info->landing = 1;
            layer_stl_shape[get_subtile_number(stl_x, stl_y)] = (unsigned short)(*col_count + 1);
            (*col_count)++;
        }
    }
}

/** Puts steps on one slab of stairs; row0 is the first row of steps on it (0 on the bottom tile, 3 on the top). */
static void shape_stairs_slab(MapSlabCoord bottom_x, MapSlabCoord bottom_y, unsigned char dir, MapSlabCoord slb_x, MapSlabCoord slb_y, int row0, int *col_count)
{
    for (int dy = 0; dy < STL_PER_SLB; dy++)
    {
        for (int dx = 0; dx < STL_PER_SLB; dx++)
        {
            if (*col_count >= LAYER_STEP_COLUMNS_MAX)
                return;
            const MapSubtlCoord stl_x = slab_subtile(slb_x, dx);
            const MapSubtlCoord stl_y = slab_subtile(slb_y, dy);
            struct Map *mapblk = get_map_block_at(stl_x, stl_y);
            struct Column *col = &layer_step_columns[*col_count];
            const struct Column *base_col = &game.columns_data[get_mapblk_column_index(mapblk)];
            const int row = row0 + stairs_row(dir, stl_x, stl_y);
            make_step_column(col, base_col, row);
            struct LayerStepInfo *info = &layer_step_info[*col_count];
            info->slb_x = bottom_x;
            info->slb_y = bottom_y;
            info->dir = dir;
            info->base_h = (unsigned char)max(1L, get_column_floor_filled_subtiles(base_col));
            info->row = (unsigned char)row;
            info->landing = 0;
            layer_stl_shape[get_subtile_number(stl_x, stl_y)] = (unsigned short)(*col_count + 1);
            (*col_count)++;
        }
    }
    if (layer_shaped_slabs_count < LAYER_SHAPED_SLABS_MAX)
    {
        struct LayerShapedSlab *item = &layer_shaped_slabs[layer_shaped_slabs_count++];
        item->slb_x = slb_x;
        item->slb_y = slb_y;
        item->dir = dir;
        item->upper = (unsigned char)row0;
    }
}

static TbBool shaped_slab_in_list(const struct LayerShapedSlab *list, int count, const struct LayerShapedSlab *item)
{
    for (int i = 0; i < count; i++)
    {
        if ((list[i].slb_x == item->slb_x) && (list[i].slb_y == item->slb_y)
         && (list[i].dir == item->dir) && (list[i].upper == item->upper))
            return true;
    }
    return false;
}

static void update_navigation_around_slab(MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    const MapSubtlCoord sx = max(0, slab_subtile(slb_x, 0) - 1);
    const MapSubtlCoord sy = max(0, slab_subtile(slb_y, 0) - 1);
    const MapSubtlCoord ex = min(game.map_subtiles_x, slab_subtile(slb_x, 0) + STL_PER_SLB + 1);
    const MapSubtlCoord ey = min(game.map_subtiles_y, slab_subtile(slb_y, 0) + STL_PER_SLB + 1);
    update_navigation_triangulation(sx, sy, ex, ey);
    // The cave ceiling opens up over stairs (see map_ceiling.c), so it follows them.
    ceiling_partially_recompute_heights(sx, sy, ex, ey);
}

static void apply_layer_shapes(TbBool update_nav)
{
    if (game.layers_count >= 2)
        ensure_layer_connectors();
    else
        layer_connectors_count = 0;
    if ((layer_shapes_generation == layer_connectors_generation) && (game.layers_count >= 2))
        return;
    layer_shapes_generation = layer_connectors_generation;
    static struct LayerShapedSlab old_slabs[LAYER_SHAPED_SLABS_MAX];
    const int old_count = layer_shaped_slabs_count;
    memcpy(old_slabs, layer_shaped_slabs, sizeof(struct LayerShapedSlab) * old_count);
    for (int i = 0; i < old_count; i++)
    {
        for (int dy = 0; dy < STL_PER_SLB; dy++)
            for (int dx = 0; dx < STL_PER_SLB; dx++)
                layer_stl_shape[get_subtile_number(slab_subtile(old_slabs[i].slb_x, dx), slab_subtile(old_slabs[i].slb_y, dy))] = 0;
        // The marks past the end of stairs reach one subtile into the tiles beside this one.
        if (old_slabs[i].upper == 0xFE)
        {
            for (int dy = -1; dy <= STL_PER_SLB; dy++)
            {
                for (int dx = -1; dx <= STL_PER_SLB; dx++)
                {
                    const MapSubtlCoord sx = slab_subtile(old_slabs[i].slb_x, 0) + dx;
                    const MapSubtlCoord sy = slab_subtile(old_slabs[i].slb_y, 0) + dy;
                    if ((sx < 0) || (sy < 0) || (sx >= game.map_subtiles_x) || (sy >= game.map_subtiles_y))
                        continue;
                    if (layer_stl_shape[get_subtile_number(sx, sy)] == LAYER_STL_BEYOND)
                        layer_stl_shape[get_subtile_number(sx, sy)] = 0;
                }
            }
        }
    }
    layer_shaped_slabs_count = 0;
    int col_count = 0;
    for (int i = 0; i < layer_connectors_count; i++)
    {
        const struct LayerConnector *conn = &layer_connectors[i];
        if (conn->kind != LCnK_Stairs)
            continue;
        int dx;
        int dy;
        dir_delta(conn->dir, &dx, &dy);
        // Steps over the bottom tile and the top one on the lower layer
        const MapSlabCoord bottom_x = conn->lower_slb_x - dx;
        const MapSlabCoord bottom_y = conn->lower_slb_y - dy;
        shape_stairs_slab(bottom_x, bottom_y, conn->dir, bottom_x, bottom_y, 0, &col_count);
        shape_stairs_slab(bottom_x, bottom_y, conn->dir, conn->lower_slb_x, conn->lower_slb_y, STL_PER_SLB, &col_count);
        // An open well above the two lower rows of the top tile, on the upper end; above the last steps
        // is the landing step, one cube under that layer's floor.
        for (int sy = 0; sy < STL_PER_SLB; sy++)
        {
            for (int sx = 0; sx < STL_PER_SLB; sx++)
            {
                const MapSubtlCoord stl_x = slab_subtile(conn->upper_slb_x, sx);
                const MapSubtlCoord stl_y = slab_subtile(conn->upper_slb_y, sy);
                if (stairs_row(conn->dir, stl_x, stl_y) < STL_PER_SLB - 1)
                    layer_stl_shape[get_subtile_number(stl_x, stl_y)] = LAYER_STL_HOLE;
            }
        }
        shape_stairs_landing(conn, &col_count);
        // Around the top row of steps: the row just past it and the floor beside it, corners included. From
        // the top row a walker only goes back down the steps or crosses to the layer above; a route that
        // leaves it sideways aims at the corner of the end, which a walker can't get to.
        for (int k = 0; k < STL_PER_SLB + 4; k++)
        {
            const int r = (k < STL_PER_SLB + 2) ? 2 * STL_PER_SLB : 2 * STL_PER_SLB - 1;
            const int a = (k < STL_PER_SLB + 2) ? k - 1 : ((k == STL_PER_SLB + 2) ? -1 : STL_PER_SLB);
            int lx;
            int ly;
            switch (conn->dir)
            {
            case LCnD_North: lx = a;                    ly = (STL_PER_SLB - 1) - r; break;
            case LCnD_East:  lx = r;                    ly = a;                     break;
            case LCnD_South: lx = (STL_PER_SLB - 1) - a; ly = r;                    break;
            default:         lx = (STL_PER_SLB - 1) - r; ly = (STL_PER_SLB - 1) - a; break;
            }
            const MapSubtlCoord stl_x = slab_subtile(bottom_x, 0) + lx;
            const MapSubtlCoord stl_y = slab_subtile(bottom_y, 0) + ly;
            if ((stl_x < 0) || (stl_y < 0) || (stl_x >= game.map_subtiles_x) || (stl_y >= game.map_subtiles_y))
                continue;
            if (layer_stl_shape[get_subtile_number(stl_x, stl_y)] == 0)
                layer_stl_shape[get_subtile_number(stl_x, stl_y)] = LAYER_STL_BEYOND;
        }
        if (layer_shaped_slabs_count < LAYER_SHAPED_SLABS_MAX)
        {
            struct LayerShapedSlab *item = &layer_shaped_slabs[layer_shaped_slabs_count++];
            item->slb_x = conn->lower_slb_x + dx;
            item->slb_y = conn->lower_slb_y + dy;
            item->dir = conn->dir;
            item->upper = 0xFE;
        }
        if (layer_shaped_slabs_count < LAYER_SHAPED_SLABS_MAX)
        {
            struct LayerShapedSlab *item = &layer_shaped_slabs[layer_shaped_slabs_count++];
            item->slb_x = conn->upper_slb_x;
            item->slb_y = conn->upper_slb_y;
            item->dir = conn->dir;
            item->upper = 0xFF;
        }
    }
    // Single stairs tiles: half stairs, the first half of the steps.
    for (int i = 0; i < layer_half_stairs_count; i++)
    {
        const struct LayerHalfStairs *half = &layer_half_stairs[i];
        shape_stairs_slab(half->slb_x, half->slb_y, half->dir, half->slb_x, half->slb_y, 0, &col_count);
    }
    if (!update_nav)
    {
        layer_shapes_nav_pending = true;
        return;
    }
    if (layer_shapes_nav_pending)
    {
        // Worked out while loading: navigation there doesn't know these shapes yet.
        layer_shapes_nav_pending = false;
        for (int i = 0; i < layer_shaped_slabs_count; i++)
            update_navigation_around_slab(layer_shaped_slabs[i].slb_x, layer_shaped_slabs[i].slb_y);
        return;
    }
    // Only the slabs whose shape changed get their navigation worked out again.
    for (int i = 0; i < old_count; i++)
    {
        if (!shaped_slab_in_list(layer_shaped_slabs, layer_shaped_slabs_count, &old_slabs[i]))
            update_navigation_around_slab(old_slabs[i].slb_x, old_slabs[i].slb_y);
    }
    for (int i = 0; i < layer_shaped_slabs_count; i++)
    {
        if (!shaped_slab_in_list(old_slabs, old_count, &layer_shaped_slabs[i]))
            update_navigation_around_slab(layer_shaped_slabs[i].slb_x, layer_shaped_slabs[i].slb_y);
    }
}

void layer_connectors_process_turn(void)
{
    if ((game.layers_count < 2) && (layer_shaped_slabs_count == 0))
        return;
    if (layer_shapes_nav_pending)
        layer_shapes_generation = 0; // shapes again, this time with navigation
    apply_layer_shapes(true);
}

void layer_connectors_init(void)
{
    memset(layer_stl_shape, 0, sizeof(layer_stl_shape));
    layer_shaped_slabs_count = 0;
    layer_connectors_changed();
    apply_layer_shapes(false);
    // The cave ceiling opens up over stairs.
    for (int i = 0; i < layer_shaped_slabs_count; i++)
    {
        const MapSubtlCoord sx = max(0, slab_subtile(layer_shaped_slabs[i].slb_x, 0) - 1);
        const MapSubtlCoord sy = max(0, slab_subtile(layer_shaped_slabs[i].slb_y, 0) - 1);
        ceiling_partially_recompute_heights(sx, sy, sx + STL_PER_SLB + 2, sy + STL_PER_SLB + 2);
    }
}

static unsigned short layer_stl_shape_at(MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if ((layer_shaped_slabs_count == 0) || (stl_x < 0) || (stl_y < 0) || (stl_x >= game.map_subtiles_x) || (stl_y >= game.map_subtiles_y))
        return 0;
    return layer_stl_shape[get_subtile_number(stl_x, stl_y)];
}

struct Column *layer_stairs_column_at(MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    const unsigned short shape = layer_stl_shape_at(stl_x, stl_y);
    if ((shape == 0) || (shape >= LAYER_STL_BEYOND))
        return NULL;
    return &layer_step_columns[shape - 1];
}

static const struct LayerStepInfo *stairs_step_info_at(MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    const unsigned short shape = layer_stl_shape_at(stl_x, stl_y);
    if ((shape == 0) || (shape >= LAYER_STL_BEYOND))
        return NULL;
    return &layer_step_info[shape - 1];
}

TbBool layer_stairs_ramp_floor(MapCoord pos_x, MapCoord pos_y, MapCoord *floor_z)
{
    const struct LayerStepInfo *info = stairs_step_info_at(coord_subtile(pos_x), coord_subtile(pos_y));
    if (info == NULL)
        return false;
    const MapCoord slab_size = STL_PER_SLB * COORD_PER_STL;
    int a;
    int r;
    stairs_local(info->dir, pos_x - (MapCoord)info->slb_x * slab_size, pos_y - (MapCoord)info->slb_y * slab_size, slab_size, &a, &r);
    if (info->landing)
    {
        // The same slope as the steps below, a storey up: walking from one to the other, the height
        // doesn't change, only the layer does.
        const MapCoord z = stairs_ramp_height(1, r + STL_PER_SLB * COORD_PER_STL) - LAYER_STOREY_CUBES * COORD_PER_STL;
        *floor_z = max((MapCoord)0, min(z, (MapCoord)COORD_PER_STL));
        return true;
    }
    *floor_z = stairs_ramp_height(info->base_h, r);
    return true;
}

MapCoord layer_stairs_entry_height(MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    const struct LayerStepInfo *info = stairs_step_info_at(stl_x, stl_y);
    if (info == NULL)
        return -1;
    if (info->landing)
        return 0;
    // A step can be walked onto from the ramp of the step before it, a creature's width away, but not
    // from the floor beside it: up to one and a half cubes under the ramp where the step starts.
    return max((MapCoord)0, stairs_ramp_height(info->base_h, info->row * COORD_PER_STL) - 3 * COORD_PER_STL / 2);
}

long layer_stairs_nav_floor_subtiles(MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    const struct LayerStepInfo *info = stairs_step_info_at(stl_x, stl_y);
    if (info == NULL)
        return -1;
    if (info->landing)
        return 0;
    // For finding routes the steps rise one cube each, the most a walker may climb at once.
    return info->base_h + 1 + info->row;
}

long layer_stairs_column_base_subtiles(const struct Column *col)
{
    if (!column_is_layer_stairs_column(col))
        return -1;
    return layer_step_info[col - &layer_step_columns[0]].base_h;
}

TbBool column_is_layer_stairs_column(const struct Column *col)
{
    return (col >= &layer_step_columns[0]) && (col < &layer_step_columns[LAYER_STEP_COLUMNS_MAX]);
}

TbBool subtile_next_to_layer_stairs(MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if (layer_shaped_slabs_count == 0)
        return false;
    for (int dy = -1; dy <= 1; dy++)
    {
        for (int dx = -1; dx <= 1; dx++)
        {
            if (layer_stairs_column_at(stl_x + dx, stl_y + dy) != NULL)
                return true;
        }
    }
    return false;
}

void layer_stairs_keep_on_top(const struct Thing *thing, struct Coord3d *nxpos)
{
    const struct LayerStepInfo *info = stairs_step_info_at(thing->mappos.x.stl.num, thing->mappos.y.stl.num);
    if ((info == NULL) || info->landing || (info->row < STL_PER_SLB))
        return;
    const MapCoord slab_size = STL_PER_SLB * COORD_PER_STL;
    const MapCoord ox = (MapCoord)info->slb_x * slab_size;
    const MapCoord oy = (MapCoord)info->slb_y * slab_size;
    int a;
    int along_now;
    int along_next;
    stairs_local(info->dir, thing->mappos.x.val - ox, thing->mappos.y.val - oy, slab_size, &a, &along_now);
    stairs_local(info->dir, nxpos->x.val - ox, nxpos->y.val - oy, slab_size, &a, &along_next);
    // As far as the end of the top step, the creature's edge included; going back is always fine.
    const int limit = 2 * slab_size - 1 - thing_nav_sizexy(thing) / 2 - 8;
    if ((along_next <= limit) || (along_next <= along_now))
        return;
    const int along_ok = max(along_now, limit);
    int dx;
    int dy;
    dir_delta(info->dir, &dx, &dy);
    nxpos->x.val += dx * (along_ok - along_next);
    nxpos->y.val += dy * (along_ok - along_next);
    // Held at the end with nowhere else to go: the route aims past it, so it is worked out again.
    if ((abs(nxpos->x.val - thing->mappos.x.val) + abs(nxpos->y.val - thing->mappos.y.val)) < 4)
        ariadne_invalidate_creature_route((struct Thing *)thing);
}

TbBool subtile_is_beyond_layer_stairs(MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    return (layer_stl_shape_at(stl_x, stl_y) == LAYER_STL_BEYOND);
}

TbBool subtile_is_layer_stairs_top_row(MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    const struct LayerStepInfo *info = stairs_step_info_at(stl_x, stl_y);
    return (info != NULL) && !info->landing && (info->row == 2 * STL_PER_SLB - 1);
}

TbBool subtile_is_layer_stairwell(MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    return (layer_stl_shape_at(stl_x, stl_y) == LAYER_STL_HOLE);
}

TbBool slab_has_layer_stairwell(MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    // The middle subtile is in the well whichever way the stairs rise.
    return subtile_is_layer_stairwell(slab_subtile_center(slb_x), slab_subtile_center(slb_y));
}

/******************************************************************************/
// Climbing between layers. A climb takes time: the creature rises (or sinks) in place on the lower
// end until it is level with the floor of the other layer, and only then moves there, so in the
// stacked view it never jumps.

/** Height gained or lost each turn while climbing, for each kind of connector. */
static const int32_t layer_climb_speed[] = {0, 64, 40, 48, 64};
#define LAYER_STOREY_HEIGHT (LAYER_STOREY_CUBES * COORD_PER_STL)

static const struct LayerConnector *connector_end_at(const struct Thing *creatng, TbBool *upper)
{
    const MapSlabCoord slb_x = coord_slab(creatng->mappos.x.val);
    const MapSlabCoord slb_y = coord_slab(creatng->mappos.y.val);
    const int idx = get_layer_connector_at_slab(slb_x, slb_y);
    if (idx < 0)
        return NULL;
    const struct LayerConnector *conn = get_layer_connector(idx);
    *upper = (conn->upper_slb_x == slb_x) && (conn->upper_slb_y == slb_y);
    return conn;
}

/** The same spot on the other end of the connector (same local subtile and offset in it). */
static void connector_other_end_pos(const struct LayerConnector *conn, TbBool from_upper, const struct Coord3d *src, struct Coord3d *dst)
{
    *dst = *src;
    int dsx = conn->upper_slb_x - conn->lower_slb_x;
    int dsy = conn->upper_slb_y - conn->lower_slb_y;
    if (from_upper)
    {
        dsx = -dsx;
        dsy = -dsy;
    }
    dst->x.val += dsx * STL_PER_SLB * COORD_PER_STL;
    dst->y.val += dsy * STL_PER_SLB * COORD_PER_STL;
}

static void stop_creature_for_climb(struct Thing *creatng)
{
    struct CreatureControl *cctrl = creature_control_get_from_thing(creatng);
    creatng->velocity.x.val = 0;
    creatng->velocity.y.val = 0;
    creatng->velocity.z.val = 0;
    creatng->veloc_base.x.val = 0;
    creatng->veloc_base.y.val = 0;
    creatng->veloc_base.z.val = 0;
    creatng->veloc_push_add.x.val = 0;
    creatng->veloc_push_add.y.val = 0;
    creatng->veloc_push_add.z.val = 0;
    clear_flag(creatng->state_flags, TF1_PushAdd);
    cctrl->moveaccel.x.val = 0;
    cctrl->moveaccel.y.val = 0;
    cctrl->moveaccel.z.val = 0;
    // Gravity leaves a thing alone at its floor height; while climbing that is where it is now.
    creatng->floor_height = creatng->mappos.z.val;
}

/** Moves the creature to the other end, keeping it at the same place in the stacked view. */
static void move_creature_to_connector_end(struct Thing *creatng, const struct LayerConnector *conn, TbBool from_upper, MapCoord z)
{
    struct Coord3d pos;
    connector_other_end_pos(conn, from_upper, &creatng->mappos, &pos);
    if (conn->kind == LCnK_Stairs)
    {
        // Keep the creature clear of what is at the end of the row it arrives on: the wall behind the top
        // step coming down, the stairwell going up. Otherwise it would be in the wall, and moved away to
        // wherever there is room.
        const MapSlabCoord slb_x = from_upper ? conn->lower_slb_x : conn->upper_slb_x;
        const MapSlabCoord slb_y = from_upper ? conn->lower_slb_y : conn->upper_slb_y;
        const MapCoord slab_size = STL_PER_SLB * COORD_PER_STL;
        int a;
        int r;
        stairs_local(conn->dir, pos.x.val - (MapCoord)slb_x * slab_size, pos.y.val - (MapCoord)slb_y * slab_size, slab_size, &a, &r);
        const int clear = thing_nav_sizexy(creatng) / 2 + 8;
        int new_r = r;
        if (from_upper)
            new_r = min(r, (int)slab_size - 1 - clear);
        else
            new_r = max(r, (STL_PER_SLB - 1) * COORD_PER_STL + clear);
        new_r = max((STL_PER_SLB - 1) * COORD_PER_STL, min(new_r, (int)slab_size - 1));
        int dx;
        int dy;
        dir_delta(conn->dir, &dx, &dy);
        pos.x.val += dx * (new_r - r);
        pos.y.val += dy * (new_r - r);
    }
    pos.z.val = z;
    move_thing_in_map(creatng, &pos);
    reset_interpolation_of_thing(creatng);
    ariadne_invalidate_creature_route(creatng);
    check_map_explored(creatng, pos.x.stl.num, pos.y.stl.num);
    SYNCDBG(8, "The %s index %d moved to layer %d at (%d,%d)", thing_model_name(creatng), (int)creatng->index,
        get_layer_at_coord(pos.x.val, pos.y.val), (int)pos.x.stl.num, (int)pos.y.stl.num);
}

TbBool creature_is_climbing_layers(const struct Thing *creatng)
{
    const struct CreatureControl *cctrl = creature_control_get_from_thing(creatng);
    if (creature_control_invalid(cctrl))
        return false;
    return (cctrl->layer_climb == LClS_Up) || (cctrl->layer_climb == LClS_Down);
}

TbBool creature_start_layer_climb(struct Thing *creatng)
{
    struct CreatureControl *cctrl = creature_control_get_from_thing(creatng);
    if (creature_control_invalid(cctrl))
        return false;
    if (creature_is_climbing_layers(creatng))
        return true;
    TbBool upper;
    const struct LayerConnector *conn = connector_end_at(creatng, &upper);
    if ((conn == NULL) || !layer_connector_usable_by(conn, creatng))
        return false;
    if (!subtile_is_connector_climb_spot(conn, upper, creatng->mappos.x.stl.num, creatng->mappos.y.stl.num))
        return false;
    if (conn->kind == LCnK_Stairs)
    {
        // Stairs are walked: the landing step on the upper end is one cube over the top step below, so
        // crossing is just a step up or down, done at once. Going down, the creature is put level with
        // where it was and drops onto the top step; going up, it stands on the landing step.
        const MapCoord z = creatng->mappos.z.val + (upper ? LAYER_STOREY_HEIGHT : -LAYER_STOREY_HEIGHT);
        move_creature_to_connector_end(creatng, conn, upper, z);
        const MapCoord floor_z = get_thing_height_at(creatng, &creatng->mappos);
        if (creatng->mappos.z.val < floor_z)
        {
            struct Coord3d pos = creatng->mappos;
            pos.z.val = floor_z;
            move_thing_in_map(creatng, &pos);
        }
        // No rest after it: which way the creature walks says whether it goes on up or comes back down.
        cctrl->layer_climb = LClS_None;
        SYNCDBG(8, "The %s index %d steps %s the stairs", thing_model_name(creatng), (int)creatng->index, upper ? "down" : "up");
        return true;
    }
    if (upper)
    {
        // Going down: move to the lower end at once, high up where the floor above is, and sink from there.
        move_creature_to_connector_end(creatng, conn, true, creatng->mappos.z.val + LAYER_STOREY_HEIGHT);
        cctrl->layer_climb = LClS_Down;
    } else
    {
        cctrl->layer_climb = LClS_Up;
    }
    creature_set_speed(creatng, 0);
    stop_creature_for_climb(creatng);
    SYNCDBG(8, "The %s index %d starts climbing %s", thing_model_name(creatng), (int)creatng->index, upper ? "down" : "up");
    return true;
}

static TbBool continue_layer_climb(struct Thing *creatng, struct CreatureControl *cctrl)
{
    TbBool upper;
    const struct LayerConnector *conn = connector_end_at(creatng, &upper);
    // Stopped if the creature is no longer on the lower end's climb spot (picked up, teleported, connector gone).
    if ((conn == NULL) || upper || !subtile_is_connector_climb_spot(conn, false, creatng->mappos.x.stl.num, creatng->mappos.y.stl.num))
    {
        cctrl->layer_climb = LClS_None;
        return false;
    }
    const int32_t speed = layer_climb_speed[conn->kind];
    struct Coord3d pos = creatng->mappos;
    if (cctrl->layer_climb == LClS_Up)
    {
        struct Coord3d top;
        connector_other_end_pos(conn, false, &creatng->mappos, &top);
        const MapCoord top_floor = get_floor_height_at(&top);
        pos.z.val += speed;
        if (pos.z.val >= top_floor + LAYER_STOREY_HEIGHT)
        {
            move_creature_to_connector_end(creatng, conn, false, top_floor);
            cctrl->layer_climb = LClS_Rest;
            stop_creature_for_climb(creatng);
            return true;
        }
    } else
    {
        const MapCoord floor = get_floor_height_at(&creatng->mappos);
        pos.z.val -= speed;
        if (pos.z.val <= floor)
        {
            pos.z.val = floor;
            cctrl->layer_climb = LClS_Rest;
        }
    }
    move_thing_in_map(creatng, &pos);
    stop_creature_for_climb(creatng);
    return true;
}

static TbBool creature_on_any_climb_spot(const struct Thing *creatng)
{
    TbBool upper;
    const struct LayerConnector *conn = connector_end_at(creatng, &upper);
    if (conn == NULL)
        return false;
    return subtile_is_connector_climb_spot(conn, upper, creatng->mappos.x.stl.num, creatng->mappos.y.stl.num);
}

/** A creature in first person climbs when it walks on the climb spot towards the other layer: up the
 * stairs on the top step, down them from the landing, or anywhere on the middle of a ladder or elevator. */
static TbBool controlled_creature_walks_into_connector(const struct Thing *creatng, const struct CreatureControl *cctrl)
{
    TbBool upper;
    const struct LayerConnector *conn = connector_end_at(creatng, &upper);
    if ((conn == NULL) || !layer_connector_usable_by(conn, creatng))
        return false;
    if (!subtile_is_connector_climb_spot(conn, upper, creatng->mappos.x.stl.num, creatng->mappos.y.stl.num))
        return false;
    int32_t mx = cctrl->moveaccel.x.val;
    int32_t my = cctrl->moveaccel.y.val;
    if ((mx == 0) && (my == 0))
    {
        mx = creatng->velocity.x.val;
        my = creatng->velocity.y.val;
    }
    if ((mx == 0) && (my == 0))
        return false;
    if (conn->kind == LCnK_Pit)
        return false;
    if (conn->kind != LCnK_Stairs)
        return true;
    int dx;
    int dy;
    dir_delta(conn->dir, &dx, &dy);
    if (upper)
    {
        dx = -dx;
        dy = -dy;
    }
    const int32_t along = mx * dx + my * dy;
    const int32_t across = abs(mx * dy - my * dx);
    return (along > 0) && (along >= across);
}

/** Moves a flier to the same spot on the layer delta storeys away, at the given height. */
static void move_flier_to_layer(struct Thing *creatng, MapSubtlCoord stl_x, MapSubtlCoord stl_y, MapCoord z)
{
    struct Coord3d pos = creatng->mappos;
    pos.x.stl.num = stl_x;
    pos.y.stl.num = stl_y;
    pos.z.val = z;
    move_thing_in_map(creatng, &pos);
    reset_interpolation_of_thing(creatng);
    ariadne_invalidate_creature_route(creatng);
    check_map_explored(creatng, stl_x, stl_y);
    SYNCDBG(8, "The flying %s index %d moved to layer %d at (%d,%d)", thing_model_name(creatng), (int)creatng->index,
        get_layer_at_coord(pos.x.val, pos.y.val), (int)stl_x, (int)stl_y);
}

/**
 * A flier in first person goes through pits: flying down to the bottom of a pit takes it into the room
 * below, and flying up into a hole in the ceiling takes it out on the layer above.
 * @return true if it moved to another layer this turn.
 */
static TbBool flier_crosses_layer_pit(struct Thing *creatng, struct CreatureControl *cctrl)
{
    if (!flag_is_set(creatng->movement_flags, TMvF_Flying) || !flag_is_set(creatng->alloc_flags, TAlF_IsControlled))
    {
        if ((cctrl->layer_climb == LClS_FlyRestAbove) || (cctrl->layer_climb == LClS_FlyRestBelow))
            cctrl->layer_climb = LClS_None;
        return false;
    }
    const MapSubtlCoord stl_x = creatng->mappos.x.stl.num;
    const MapSubtlCoord stl_y = creatng->mappos.y.stl.num;
    const MapCoord z = creatng->mappos.z.val;
    const MapCoord ceiling = get_ceiling_height_at(&creatng->mappos);
    // Just arrived: wait until it moved away from the hole it came through.
    if (cctrl->layer_climb == LClS_FlyRestAbove)
    {
        if ((z >= COORD_PER_STL) || !subtile_is_layer_fall_pit(stl_x, stl_y))
            cctrl->layer_climb = LClS_None;
        return false;
    }
    if (cctrl->layer_climb == LClS_FlyRestBelow)
    {
        if ((z + creatng->clipbox_size_z + COORD_PER_STL <= ceiling) || !subtile_is_below_layer_pit(stl_x, stl_y))
            cctrl->layer_climb = LClS_None;
        return false;
    }
    if (cctrl->layer_climb != LClS_None)
        return false;
    // Down: at the bottom of a pit, into the room below, as high up as it fits under that room's ceiling.
    if ((z <= COORD_PER_STL / 8) && subtile_is_layer_fall_pit(stl_x, stl_y))
    {
        MapSubtlCoord bx = stl_x;
        MapSubtlCoord by = stl_y;
        if (get_subtile_below_layer_pit(&bx, &by))
        {
            const MapCoord below_floor = get_floor_height(bx, by);
            const MapCoord below_ceiling = get_ceiling_height_at_subtile(bx, by);
            const MapCoord below_z = max(below_floor, min(z + LAYER_STOREY_HEIGHT, below_ceiling - creatng->clipbox_size_z - COORD_PER_STL / 4));
            move_flier_to_layer(creatng, bx, by, below_z);
            cctrl->layer_climb = LClS_FlyRestBelow;
            return true;
        }
    }
    // Up: against the ceiling under a hole, out at the bottom of the pit above.
    if ((z + creatng->clipbox_size_z + COORD_PER_STL / 2 >= ceiling) && subtile_is_below_layer_pit(stl_x, stl_y))
    {
        MapSubtlCoord ax = stl_x;
        MapSubtlCoord ay = stl_y;
        if (get_subtile_above_layer(&ax, &ay) && subtile_is_layer_fall_pit(ax, ay))
        {
            move_flier_to_layer(creatng, ax, ay, max(0, z - LAYER_STOREY_HEIGHT));
            cctrl->layer_climb = LClS_FlyRestAbove;
            return true;
        }
    }
    return false;
}

TbBool update_creature_layer_climb(struct Thing *creatng)
{
    struct CreatureControl *cctrl = creature_control_get_from_thing(creatng);
    if (creature_control_invalid(cctrl))
        return false;
    if (!map_has_layers())
    {
        cctrl->layer_climb = LClS_None;
        return false;
    }
    if (flier_crosses_layer_pit(creatng, cctrl))
        return false;
    if ((cctrl->layer_climb == LClS_FlyRestAbove) || (cctrl->layer_climb == LClS_FlyRestBelow))
        return false;
    switch (cctrl->layer_climb)
    {
    case LClS_Up:
    case LClS_Down:
        return continue_layer_climb(creatng, cctrl);
    case LClS_Rest:
        if (!creature_on_any_climb_spot(creatng))
            cctrl->layer_climb = LClS_None;
        return false;
    default:
        if (!flag_is_set(creatng->alloc_flags, TAlF_IsControlled))
            return false;
        if (!controlled_creature_walks_into_connector(creatng, cctrl))
            return false;
        if (!creature_start_layer_climb(creatng))
            return false;
        if (!creature_is_climbing_layers(creatng))
            return true;
        return continue_layer_climb(creatng, cctrl);
    }
}

/******************************************************************************/
#ifdef __cplusplus
}
#endif
