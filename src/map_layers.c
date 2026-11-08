/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file map_layers.c
 *     Dungeon layers: separate areas of one map used as floors.
 * @par Purpose:
 *     Loads the layer rectangles of a level, answers which layer a position
 *     is on, and maps a position on one layer to the same spot on another.
 * @par Comment:
 *     Layers are kept in the game state (struct Game), so they are saved
 *     and synced with everything else.
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "map_layers.h"

#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "config_campaigns.h"
#include "game_legacy.h"
#include "map_data.h"
#include "map_blocks.h"
#include "map_columns.h"
#include "slab_data.h"
#include "config_terrain.h"
#include "ariadne.h"
#include "ariadne_update.h"
#include "thing_data.h"
#include "thing_list.h"
#include "thing_navigate.h"
#include "thing_creature.h"
#include "bflib_sound.h"
#include "map_connectors.h"
#include "map_layer_dig.h"
#include "frontmenu_ingame_map.h"
#include "ariadne_update.h"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
static const char *default_layer_names[MAX_MAP_LAYERS] = {"Ground", "+1", "+2", "+3"};
/******************************************************************************/

TbBool parse_level_layer_line(struct LevelInformation *lvinfo, const char *buf, int32_t *pos, long len, const char *fname)
{
    char word_buf[MAP_LAYER_NAME_LEN];
    int32_t vals[4];
    for (int i = 0; i < 4; i++)
    {
        if (get_conf_parameter_single(buf, pos, len, word_buf, sizeof(word_buf)) <= 0)
        {
            WARNMSG("LAYER line in '%s' needs four numbers: x0 y0 x1 y1.", fname);
            return false;
        }
        vals[i] = atoi(word_buf);
    }
    if (lvinfo->layers_count >= MAX_MAP_LAYERS)
    {
        WARNMSG("Too many LAYER lines in '%s'; at most %d are allowed.", fname, MAX_MAP_LAYERS);
        return false;
    }
    struct MapLayer *layer = &lvinfo->layers[lvinfo->layers_count];
    memset(layer, 0, sizeof(*layer));
    layer->x0 = (int16_t)min(vals[0], vals[2]);
    layer->y0 = (int16_t)min(vals[1], vals[3]);
    layer->x1 = (int16_t)max(vals[0], vals[2]);
    layer->y1 = (int16_t)max(vals[1], vals[3]);
    // The name is optional and may contain spaces.
    if (get_conf_parameter_whole(buf, pos, len, layer->name, sizeof(layer->name)) <= 0)
        layer->name[0] = '\0';
    lvinfo->layers_count++;
    return true;
}

static TbBool layers_overlap(const struct MapLayer *a, const struct MapLayer *b)
{
    return (a->x0 <= b->x1) && (b->x0 <= a->x1) && (a->y0 <= b->y1) && (b->y0 <= a->y1);
}

/** Checks a layer against the map and the layers already accepted. */
static TbBool layer_is_valid(const struct MapLayer *layer, int index)
{
    if ((layer->x0 < 0) || (layer->y0 < 0) || (layer->x1 >= game.map_tiles_x) || (layer->y1 >= game.map_tiles_y))
    {
        WARNLOG("Layer %d (%d,%d)-(%d,%d) is outside the %dx%d map; ignored.", index,
            (int)layer->x0, (int)layer->y0, (int)layer->x1, (int)layer->y1, (int)game.map_tiles_x, (int)game.map_tiles_y);
        return false;
    }
    for (int i = 0; i < game.layers_count; i++)
    {
        const struct MapLayer *other = &game.layers[i];
        if (layers_overlap(layer, other))
        {
            WARNLOG("Layer %d overlaps layer %d; ignored.", index, i);
            return false;
        }
        if (((layer->x1 - layer->x0) != (other->x1 - other->x0)) || ((layer->y1 - layer->y0) != (other->y1 - other->y0)))
        {
            WARNLOG("Layer %d is not the same size as layer %d; all layers must match so floors line up. Ignored.", index, i);
            return false;
        }
    }
    return true;
}

/**
 * Sound: hears an emitter on another layer as if the layers were stacked. Its position moves to the
 * same spot on the receiver's layer, a storey away per layer of difference (always upwards, since only
 * the distance counts and sound coordinates are unsigned).
 */
long layer_sound_position_map(long recv_x, long recv_y, long recv_z, long *emit_x, long *emit_y, long *emit_z)
{
    if (game.layers_count < 2)
        return 0;
    const int recv_layer = get_layer_at_coord(recv_x, recv_y);
    const int emit_layer = get_layer_at_coord(*emit_x, *emit_y);
    if ((recv_layer < 0) || (emit_layer < 0) || (recv_layer == emit_layer))
        return 0;
    MapCoord pos_x = *emit_x;
    MapCoord pos_y = *emit_y;
    if (!translate_coord_to_layer(emit_layer, recv_layer, &pos_x, &pos_y))
        return 0;
    *emit_x = pos_x;
    *emit_y = pos_y;
    // The distance is the real height difference plus a short extra per storey, for the rock in between.
    // Coordinates are unsigned, so the emitter is placed that far above the receiver; only the distance counts.
    *emit_z = recv_z + labs(recv_z - *emit_z) + (long)abs(recv_layer - emit_layer) * LAYER_SOUND_STOREY_DISTANCE;
    return 1;
}

void init_map_layers(LevelNumber lvnum)
{
    game.layers_count = 0;
    memset(game.layers, 0, sizeof(game.layers));
    clear_layer_floors();
    struct LevelInformation *lvinfo = get_level_info(lvnum);
    if (lvinfo == NULL)
        return;
    for (int i = 0; i < lvinfo->layers_count; i++)
    {
        const struct MapLayer *layer = &lvinfo->layers[i];
        if (!layer_is_valid(layer, i))
            continue;
        struct MapLayer *dst = &game.layers[game.layers_count];
        *dst = *layer;
        if (dst->name[0] == '\0')
            snprintf(dst->name, sizeof(dst->name), "%s", default_layer_names[game.layers_count]);
        game.layers_count++;
    }
    if (game.layers_count > 0)
    {
        JUSTLOG("Level %d has %d dungeon layers.", (int)lvnum, (int)game.layers_count);
    }
}

int get_map_layers_count(void)
{
    return (game.layers_count > 0) ? game.layers_count : 1;
}

TbBool map_has_layers(void)
{
    return (game.layers_count > 1);
}

int get_layer_at_slab(MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    if (game.layers_count == 0)
        return 0;
    for (int i = 0; i < game.layers_count; i++)
    {
        const struct MapLayer *layer = &game.layers[i];
        if ((slb_x >= layer->x0) && (slb_x <= layer->x1) && (slb_y >= layer->y0) && (slb_y <= layer->y1))
            return i;
    }
    return -1;
}

int get_layer_at_subtile(MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    return get_layer_at_slab(subtile_slab(stl_x), subtile_slab(stl_y));
}

int get_layer_at_coord(MapCoord pos_x, MapCoord pos_y)
{
    if ((pos_x < 0) || (pos_y < 0))
        return -1;
    return get_layer_at_subtile(coord_subtile(pos_x), coord_subtile(pos_y));
}

const char *get_layer_name(int layer)
{
    if ((layer >= 0) && (layer < game.layers_count))
        return game.layers[layer].name;
    if ((layer >= 0) && (layer < MAX_MAP_LAYERS))
        return default_layer_names[layer];
    return "?";
}

TbBool translate_coord_to_layer(int from_layer, int to_layer, MapCoord *pos_x, MapCoord *pos_y)
{
    if ((from_layer < 0) || (from_layer >= game.layers_count) || (to_layer < 0) || (to_layer >= game.layers_count))
        return false;
    const struct MapLayer *from = &game.layers[from_layer];
    const struct MapLayer *to = &game.layers[to_layer];
    *pos_x += (MapCoord)(to->x0 - from->x0) * COORD_PER_SLB;
    *pos_y += (MapCoord)(to->y0 - from->y0) * COORD_PER_SLB;
    return true;
}

void clamp_view_position_to_layer(MapCoord from_x, MapCoord from_y, MapCoord *pos_x, MapCoord *pos_y)
{
    if (game.layers_count == 0)
        return;
    const int layer_idx = get_layer_at_coord(from_x, from_y);
    if (layer_idx < 0)
        return;
    const struct MapLayer *layer = &game.layers[layer_idx];
    const MapCoord min_x = (MapCoord)layer->x0 * COORD_PER_SLB;
    const MapCoord min_y = (MapCoord)layer->y0 * COORD_PER_SLB;
    const MapCoord max_x = (MapCoord)(layer->x1 + 1) * COORD_PER_SLB - 1;
    const MapCoord max_y = (MapCoord)(layer->y1 + 1) * COORD_PER_SLB - 1;
    *pos_x = clamp(*pos_x, min_x, max_x);
    *pos_y = clamp(*pos_y, min_y, max_y);
}

/******************************************************************************/

/** Open means a creature could stand there: not rock, earth, a wall, gold or a door.
 * Rooms are open: they are flagged blocking only for their pillar columns. */
static TbBool slab_is_open_for_pit(MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    struct SlabMap *slb = get_slabmap_block(slb_x, slb_y);
    if (slabmap_block_invalid(slb))
        return false;
    const struct SlabConfigStats *slabst = get_slab_stats(slb);
    if ((slabst->block_flags & SlbAtFlg_IsDoor) != 0)
        return false;
    if ((slabst->block_flags & SlbAtFlg_IsRoom) != 0)
        return true;
    return (slabst->block_flags & SlbAtFlg_Blocking) == 0;
}

/** Set below; cleared when a floor is dug so the pit distances are worked out again. */
static TbBool layer_pit_distance_valid;

TbBool slab_is_layer_pit(MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    if (game.layers_count < 2)
        return false;
    const int layer = get_layer_at_slab(slb_x, slb_y);
    if (layer <= 0)
        return false;
    if (!slab_is_open_for_pit(slb_x, slb_y))
        return false;
    // A bridge spans the hole, as it does over lava or an abyss.
    struct SlabMap *slb = get_slabmap_block(slb_x, slb_y);
    if (get_slab_stats(slb)->wlb_type == WlbT_Bridge)
        return false;
    // The top end of stairs or an elevator is a floor, not a hole.
    if (room_kind_layer_connector_kind(slab_corresponding_room(slb->kind)) != LCnK_None)
        return false;
    // Every tile has a floor between it and the layer below; only where it was dug out is there a hole.
    return layer_floor_is_dug(slb_x, slb_y);
}

TbBool slab_is_layer_fall_pit(MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    if (!slab_is_layer_pit(slb_x, slb_y))
        return false;
    const int layer = get_layer_at_slab(slb_x, slb_y);
    const MapSlabCoord below_x = slb_x + game.layers[layer - 1].x0 - game.layers[layer].x0;
    const MapSlabCoord below_y = slb_y + game.layers[layer - 1].y0 - game.layers[layer].y0;
    return slab_is_open_for_pit(below_x, below_y);
}

TbBool subtile_is_layer_fall_pit(MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if (game.layers_count < 2)
        return false;
    if (subtile_is_layer_stairwell(stl_x, stl_y))
        return true;
    return slab_is_layer_fall_pit(subtile_slab(stl_x), subtile_slab(stl_y));
}

void layer_floor_changed(MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    if (game.layers_count < 2)
        return;
    layer_pit_distance_valid = false;
    layer_connectors_changed();
    const int layer = get_layer_at_slab(slb_x, slb_y);
    for (int delta = 0; delta >= -1; delta--)
    {
        const int other = layer + delta;
        if ((layer < 0) || (other < 0))
            continue;
        const MapSlabCoord ox = slb_x + game.layers[other].x0 - game.layers[layer].x0;
        const MapSlabCoord oy = slb_y + game.layers[other].y0 - game.layers[layer].y0;
        const MapSubtlCoord sx = max(0, slab_subtile(ox, 0) - STL_PER_SLB);
        const MapSubtlCoord sy = max(0, slab_subtile(oy, 0) - STL_PER_SLB);
        const MapSubtlCoord ex = min(game.map_subtiles_x, slab_subtile(ox, 0) + 2 * STL_PER_SLB);
        const MapSubtlCoord ey = min(game.map_subtiles_y, slab_subtile(oy, 0) + 2 * STL_PER_SLB);
        update_navigation_triangulation(sx, sy, ex, ey);
        panel_map_update(slab_subtile(ox, 0), slab_subtile(oy, 0), STL_PER_SLB, STL_PER_SLB);
    }
}

TbBool subtile_is_layer_pit(MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if (game.layers_count < 2)
        return false;
    // The open well above stairs, on the layer above them.
    if (subtile_is_layer_stairwell(stl_x, stl_y))
        return true;
    return slab_is_layer_pit(subtile_slab(stl_x), subtile_slab(stl_y));
}

TbBool get_subtile_above_layer(MapSubtlCoord *stl_x, MapSubtlCoord *stl_y)
{
    if (game.layers_count < 2)
        return false;
    const int layer = get_layer_at_subtile(*stl_x, *stl_y);
    if ((layer < 0) || (layer + 1 >= game.layers_count))
        return false;
    *stl_x += (MapSubtlCoord)(game.layers[layer + 1].x0 - game.layers[layer].x0) * STL_PER_SLB;
    *stl_y += (MapSubtlCoord)(game.layers[layer + 1].y0 - game.layers[layer].y0) * STL_PER_SLB;
    return true;
}

TbBool get_subtile_layers_below(MapSubtlCoord *stl_x, MapSubtlCoord *stl_y, int storeys)
{
    if ((game.layers_count < 2) || (storeys < 0))
        return false;
    const int layer = get_layer_at_subtile(*stl_x, *stl_y);
    if ((layer < 0) || (layer - storeys < 0))
        return false;
    *stl_x += (MapSubtlCoord)(game.layers[layer - storeys].x0 - game.layers[layer].x0) * STL_PER_SLB;
    *stl_y += (MapSubtlCoord)(game.layers[layer - storeys].y0 - game.layers[layer].y0) * STL_PER_SLB;
    return true;
}

TbBool get_subtile_below_layer(MapSubtlCoord *stl_x, MapSubtlCoord *stl_y)
{
    if (game.layers_count < 2)
        return false;
    const int layer = get_layer_at_subtile(*stl_x, *stl_y);
    if (layer <= 0)
        return false;
    *stl_x += (MapSubtlCoord)(game.layers[layer - 1].x0 - game.layers[layer].x0) * STL_PER_SLB;
    *stl_y += (MapSubtlCoord)(game.layers[layer - 1].y0 - game.layers[layer].y0) * STL_PER_SLB;
    return true;
}

TbBool subtile_is_below_layer_pit(MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if (!get_subtile_above_layer(&stl_x, &stl_y))
        return false;
    // Only a hole over open ground opens the ceiling; a dug floor over a wall leaves the wall's ceiling whole.
    return subtile_is_layer_fall_pit(stl_x, stl_y);
}

/******************************************************************************/
// Distance from each slab to the nearest pit on its layer, for rendering only.
#define PIT_DISTANCE_RANGE (max(LAYER_ABOVE_VIEW_RADIUS, LAYER_BELOW_VIEW_RADIUS + 1))
static unsigned char layer_pit_distance[MAX_TILES_X * MAX_TILES_Y];
static GameTurn layer_pit_distance_turn = 0;
static TbBool layer_pit_distance_valid = false;

static void refresh_layer_pit_distance(void)
{
    if (layer_pit_distance_valid && (layer_pit_distance_turn == get_gameturn()))
        return;
    memset(layer_pit_distance, 255, sizeof(layer_pit_distance));
    for (int layer = 1; layer < game.layers_count; layer++)
    {
        const struct MapLayer *ml = &game.layers[layer];
        for (MapSlabCoord slb_y = ml->y0; slb_y <= ml->y1; slb_y++)
        {
            for (MapSlabCoord slb_x = ml->x0; slb_x <= ml->x1; slb_x++)
            {
                if (!slab_is_layer_pit(slb_x, slb_y) && !slab_has_layer_stairwell(slb_x, slb_y))
                    continue;
                for (int dy = -PIT_DISTANCE_RANGE; dy <= PIT_DISTANCE_RANGE; dy++)
                {
                    for (int dx = -PIT_DISTANCE_RANGE; dx <= PIT_DISTANCE_RANGE; dx++)
                    {
                        const int nx = slb_x + dx;
                        const int ny = slb_y + dy;
                        if ((nx < ml->x0) || (nx > ml->x1) || (ny < ml->y0) || (ny > ml->y1))
                            continue;
                        const unsigned char dist = (unsigned char)max(abs(dx), abs(dy));
                        unsigned char *cur = &layer_pit_distance[get_slab_number(nx, ny)];
                        if (dist < *cur)
                            *cur = dist;
                    }
                }
            }
        }
    }
    layer_pit_distance_turn = get_gameturn();
    layer_pit_distance_valid = true;
}

int subtile_layer_pit_distance(MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if (game.layers_count < 2)
        return 255;
    const MapSlabCoord slb_x = subtile_slab(stl_x);
    const MapSlabCoord slb_y = subtile_slab(stl_y);
    if ((slb_x < 0) || (slb_y < 0) || (slb_x >= game.map_tiles_x) || (slb_y >= game.map_tiles_y))
        return 255;
    refresh_layer_pit_distance();
    return layer_pit_distance[get_slab_number(slb_x, slb_y)];
}

TbBool subtile_near_layer_pit(MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    return subtile_layer_pit_distance(stl_x, stl_y) <= LAYER_BELOW_VIEW_RADIUS;
}

TbBool subtile_near_layer_pit_above(MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if (!get_subtile_above_layer(&stl_x, &stl_y))
        return false;
    return subtile_layer_pit_distance(stl_x, stl_y) <= LAYER_ABOVE_VIEW_RADIUS;
}

TbBool get_subtile_below_layer_pit(MapSubtlCoord *stl_x, MapSubtlCoord *stl_y)
{
    if (!subtile_is_layer_fall_pit(*stl_x, *stl_y))
        return false;
    const int layer = get_layer_at_subtile(*stl_x, *stl_y);
    *stl_x += (MapSubtlCoord)(game.layers[layer - 1].x0 - game.layers[layer].x0) * STL_PER_SLB;
    *stl_y += (MapSubtlCoord)(game.layers[layer - 1].y0 - game.layers[layer].y0) * STL_PER_SLB;
    return true;
}

void update_layer_above_slab(MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    if (game.layers_count < 2)
        return;
    layer_pit_distance_valid = false;
    layer_connectors_changed();
    const int layer = get_layer_at_slab(slb_x, slb_y);
    if ((layer < 0) || (layer + 1 >= game.layers_count))
        return;
    const MapSlabCoord above_x = slb_x + game.layers[layer + 1].x0 - game.layers[layer].x0;
    const MapSlabCoord above_y = slb_y + game.layers[layer + 1].y0 - game.layers[layer].y0;
    const MapSubtlCoord sx = max(0, slab_subtile(above_x, 0) - STL_PER_SLB);
    const MapSubtlCoord sy = max(0, slab_subtile(above_y, 0) - STL_PER_SLB);
    const MapSubtlCoord ex = min(game.map_subtiles_x, slab_subtile(above_x, 0) + 2 * STL_PER_SLB);
    const MapSubtlCoord ey = min(game.map_subtiles_y, slab_subtile(above_y, 0) + 2 * STL_PER_SLB);
    update_navigation_triangulation(sx, sy, ex, ey);
}

TbBool drop_thing_through_layer_pit(struct Thing *thing)
{
    MapSubtlCoord stl_x = thing->mappos.x.stl.num;
    MapSubtlCoord stl_y = thing->mappos.y.stl.num;
    if (!get_subtile_below_layer_pit(&stl_x, &stl_y))
        return false;
    // Switch layers once the thing is below the ceiling of the room under the pit, keeping its height
    // (one storey up) and its speed, so the fall looks like one smooth drop.
    const MapCoord ceiling = get_ceiling_height_at_subtile(stl_x, stl_y);
    const MapCoord below_z = thing->mappos.z.val + LAYER_STOREY_CUBES * COORD_PER_STL;
    if (below_z > ceiling - thing->clipbox_size_z)
        return false;
    struct Coord3d pos = thing->mappos;
    pos.x.stl.num = stl_x;
    pos.y.stl.num = stl_y;
    pos.z.val = max(get_floor_height(stl_x, stl_y), below_z);
    clear_flag(thing->state_flags, TF1_FallingIntoAbyss);
    move_thing_in_map(thing, &pos);
    reset_interpolation_of_thing(thing);
    if (thing_is_creature(thing))
    {
        ariadne_invalidate_creature_route(thing);
        check_map_explored(thing, stl_x, stl_y);
    }
    SYNCDBG(8, "Thing %d fell through a pit to (%d,%d)", (int)thing->index, (int)stl_x, (int)stl_y);
    return true;
}

/******************************************************************************/
#ifdef __cplusplus
}
#endif
