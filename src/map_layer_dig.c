/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file map_layer_dig.c
 *     Dungeon layers: digging through the floor between two layers.
 * @par Purpose:
 *     Every tile on a layer above Ground has a floor under it. Imps can dig it
 *     out from above (standing beside the tile) or from below (standing under
 *     it). Once a floor is gone, the wall on the other side of it can be dug
 *     through the hole too. A dug floor over open ground is a pit.
 * @par Comment:
 *     Floor tasks live in the usual dig task list with their own kind, so the
 *     imps' dig states work them like walls. A floor task is keyed by the
 *     tile's first subtile, so it never clashes with a wall task on the same
 *     tile (those use the middle subtile).
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "map_layer_dig.h"
#include <string.h>
#include "globals.h"
#include "game_legacy.h"
#include "map_layers.h"
#include "map_connectors.h"
#include "map_data.h"
#include "map_blocks.h"
#include "map_utils.h"
#include "slab_data.h"
#include "config_terrain.h"
#include "tasks_list.h"
#include "spdigger_stack.h"
#include "dungeon_data.h"
#include "player_data.h"
#include "thing_traps.h"
#include "thing_list.h"
#include "frontmenu_ingame_map.h"
#include "gui_soundmsgs.h"
#include "engine_render.h"
#include "config_magic.h"
#include "config_sounds.h"
#include "thing_effects.h"
#include "thing_stats.h"
#include "sounds.h"
#include "creature_states.h"
#include "thing_shots.h"
#include "engine_textures.h"
#include "front_simple.h"
#include <stdlib.h>
#include <limits.h>
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
enum LayerVerticalDigTargets {
    LVDig_None = 0,
    LVDig_Floor,
    LVDig_Wall,
};

void clear_layer_floors(void)
{
    memset(game.layer_floor_dug, 0, sizeof(game.layer_floor_dug));
    memset(game.layer_floor_damage, 0, sizeof(game.layer_floor_damage));
}

TbBool layer_floor_is_dug(MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    if (game.layers_count < 2)
        return false;
    if ((slb_x < 0) || (slb_y < 0) || (slb_x >= game.map_tiles_x) || (slb_y >= game.map_tiles_y))
        return false;
    if (get_layer_at_slab(slb_x, slb_y) <= 0)
        return false;
    return (game.layer_floor_dug[get_slab_number(slb_x, slb_y)] != 0);
}

/** The same local tile on the layer delta storeys away. */
static TbBool slab_on_other_layer(MapSlabCoord slb_x, MapSlabCoord slb_y, int delta, MapSlabCoord *out_x, MapSlabCoord *out_y)
{
    const int layer = get_layer_at_slab(slb_x, slb_y);
    const int other = layer + delta;
    if ((layer < 0) || (other < 0) || (other >= game.layers_count))
        return false;
    *out_x = slb_x + game.layers[other].x0 - game.layers[layer].x0;
    *out_y = slb_y + game.layers[other].y0 - game.layers[layer].y0;
    return true;
}

/** A tile a creature can stand on: floor or a room, not a wall, rock, gold or a door. */
static TbBool slab_is_open_tile(MapSlabCoord slb_x, MapSlabCoord slb_y)
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

static TbBool slab_is_diggable_wall_for(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    if (slab_is_open_tile(slb_x, slb_y))
        return false;
    return subtile_is_diggable_for_player(plyr_idx, slab_subtile(slb_x, 0), slab_subtile(slb_y, 0), false);
}

/** Whether the floor under a tile may be dug: not under a room, door, trap, liquid or bridge.
 * From above the tile must be the player's floor or a bare path; from below it may also be a wall. */
static TbBool layer_floor_can_be_dug(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y, TbBool from_below)
{
    struct SlabMap *slb = get_slabmap_block(slb_x, slb_y);
    if (slabmap_block_invalid(slb) || (get_layer_at_slab(slb_x, slb_y) <= 0))
        return false;
    const struct SlabConfigStats *slabst = get_slab_stats(slb);
    if ((slabst->block_flags & (SlbAtFlg_IsRoom | SlbAtFlg_IsDoor)) != 0)
        return false;
    if (slab_kind_is_liquid(slb->kind) || (slabst->wlb_type == WlbT_Bridge))
        return false;
    if (slab_has_trap_on(slb_x, slb_y) || slab_has_door_thing_on(slb_x, slb_y))
        return false;
    if (slb->kind == SlbT_PATH)
        return true;
    if ((slb->kind == SlbT_CLAIMED) && (slabmap_owner(slb) == plyr_idx))
        return true;
    return from_below && ((slabst->block_flags & SlbAtFlg_Blocking) != 0);
}

/** Works out what a click would dig: the floor below (or above) the clicked tile, or the wall behind it. */
static int resolve_vertical_dig(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y, TbBool up, MapSlabCoord *tx, MapSlabCoord *ty)
{
    if (!map_has_layers() || slab_coords_invalid(slb_x, slb_y))
        return LVDig_None;
    if (!subtile_revealed(slab_subtile_center(slb_x), slab_subtile_center(slb_y), plyr_idx))
        return LVDig_None;
    // Walls are tagged the usual way; this is for clicks on open floor.
    if (!slab_is_open_tile(slb_x, slb_y))
        return LVDig_None;
    MapSlabCoord fx = slb_x;
    MapSlabCoord fy = slb_y;
    if (up)
    {
        if (!slab_on_other_layer(slb_x, slb_y, 1, &fx, &fy))
            return LVDig_None;
    } else
    {
        if (get_layer_at_slab(slb_x, slb_y) <= 0)
            return LVDig_None;
        // A room, a connector or anything built on the tile keeps its floor.
        struct SlabMap *slb = get_slabmap_block(slb_x, slb_y);
        if ((get_slab_stats(slb)->block_flags & SlbAtFlg_IsRoom) != 0)
            return LVDig_None;
    }
    if (!layer_floor_is_dug(fx, fy))
    {
        if (!layer_floor_can_be_dug(plyr_idx, fx, fy, up))
            return LVDig_None;
        *tx = fx;
        *ty = fy;
        return LVDig_Floor;
    }
    // The floor is already gone: the wall on the other side of it.
    MapSlabCoord wx = fx;
    MapSlabCoord wy = fy;
    if (!up && !slab_on_other_layer(fx, fy, -1, &wx, &wy))
        return LVDig_None;
    if (!slab_is_diggable_wall_for(plyr_idx, wx, wy))
        return LVDig_None;
    *tx = wx;
    *ty = wy;
    return LVDig_Wall;
}

static SubtlCodedCoords layer_floor_task_stl(MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    return get_subtile_number(slab_subtile(slb_x, 0), slab_subtile(slb_y, 0));
}

/** Adds a floor task keyed by the tile's first subtile. add_task_list_entry() can't be used: it moves
 * every task to the middle subtile, where it would clash with a wall task on the same tile. */
static void add_layer_floor_task(PlayerNumber plyr_idx, SubtlCodedCoords stl_num)
{
    struct Dungeon *dungeon = get_dungeon(plyr_idx);
    if (dungeon_invalid(dungeon))
        return;
    int task_idx;
    for (task_idx = 0; task_idx < dungeon->highest_task_number; task_idx++)
    {
        if (dungeon->task_list[task_idx].kind == SDDigTask_None)
            break;
    }
    if (task_idx == dungeon->highest_task_number)
    {
        if (task_idx >= MAPTASKS_COUNT)
            return;
        dungeon->highest_task_number++;
    }
    struct MapTask *mtask = &dungeon->task_list[task_idx];
    mtask->kind = SDDigTask_DigLayerFloor;
    mtask->coords = stl_num;
    dungeon->task_count++;
}

/** Shows (or hides) a floor tag to the local player: on the tile and on the tile under it. */
static void show_layer_floor_tag(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y, TbBool tagged)
{
    if (!is_my_player_number(plyr_idx))
        return;
    MapSlabCoord tiles_x[2] = {slb_x, slb_x};
    MapSlabCoord tiles_y[2] = {slb_y, slb_y};
    const int count = slab_on_other_layer(slb_x, slb_y, -1, &tiles_x[1], &tiles_y[1]) ? 2 : 1;
    for (int i = 0; i < count; i++)
    {
        for (int dy = 0; dy < STL_PER_SLB; dy++)
        {
            for (int dx = 0; dx < STL_PER_SLB; dx++)
            {
                struct Map *mapblk = get_map_block_at(slab_subtile(tiles_x[i], dx), slab_subtile(tiles_y[i], dy));
                if (map_block_invalid(mapblk) || ((mapblk->flags & SlbAtFlg_Blocking) != 0))
                    continue;
                if (tagged)
                    mapblk->flags |= SlbAtFlg_Unexplored;
                else
                    mapblk->flags &= ~SlbAtFlg_Unexplored;
            }
        }
        panel_map_update(slab_subtile(tiles_x[i], 0), slab_subtile(tiles_y[i], 0), STL_PER_SLB, STL_PER_SLB);
    }
}

TbBool layer_vertical_dig_click_possible(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y, TbBool up)
{
    MapSlabCoord tx;
    MapSlabCoord ty;
    return (resolve_vertical_dig(plyr_idx, slb_x, slb_y, up, &tx, &ty) != LVDig_None);
}

int layer_vertical_dig_tag_state(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y, TbBool up)
{
    MapSlabCoord tx = 0;
    MapSlabCoord ty = 0;
    switch (resolve_vertical_dig(plyr_idx, slb_x, slb_y, up, &tx, &ty))
    {
    case LVDig_Floor:
        return (find_dig_from_task_list(plyr_idx, layer_floor_task_stl(tx, ty)) >= 0) ? 1 : 0;
    case LVDig_Wall:
        return (find_from_task_list_by_slab(plyr_idx, tx, ty) != -1) ? 1 : 0;
    default:
        return -1;
    }
}

TbBool layer_toggle_vertical_dig(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y, TbBool up)
{
    return layer_set_vertical_dig(plyr_idx, slb_x, slb_y, up, LVDigM_Toggle);
}

TbBool layer_set_vertical_dig(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y, TbBool up, int mode)
{
    MapSlabCoord tx = 0;
    MapSlabCoord ty = 0;
    switch (resolve_vertical_dig(plyr_idx, slb_x, slb_y, up, &tx, &ty))
    {
    case LVDig_Floor:
    {
        const SubtlCodedCoords stl_num = layer_floor_task_stl(tx, ty);
        const long task_idx = find_dig_from_task_list(plyr_idx, stl_num);
        // A drag or an area only tags (or only untags): leave tiles that already are the way it wants.
        if ((task_idx >= 0) && (mode == LVDigM_Tag))
            return false;
        if ((task_idx < 0) && (mode == LVDigM_Untag))
            return false;
        if (task_idx >= 0)
        {
            remove_from_task_list(plyr_idx, task_idx);
            show_layer_floor_tag(plyr_idx, tx, ty, false);
            JUSTLOG("Dungeon layers: player %d untagged the floor under slab (%d,%d)", (int)plyr_idx, (int)tx, (int)ty);
            return true;
        }
        struct Dungeon *dungeon = get_dungeon(plyr_idx);
        if (dungeon->task_count >= MAPTASKS_COUNT)
        {
            if (is_my_player_number(plyr_idx))
                output_message(SMsg_WorkerJobsLimit, 500);
            return false;
        }
        add_layer_floor_task(plyr_idx, stl_num);
        show_layer_floor_tag(plyr_idx, tx, ty, true);
        JUSTLOG("Dungeon layers: player %d tagged the floor under slab (%d,%d)", (int)plyr_idx, (int)tx, (int)ty);
        return true;
    }
    case LVDig_Wall:
    {
        const TbBool tagged = (find_from_task_list_by_slab(plyr_idx, tx, ty) != -1);
        if ((tagged && (mode == LVDigM_Tag)) || (!tagged && (mode == LVDigM_Untag)))
            return false;
        if (tagged)
            return untag_blocks_for_digging_in_area(slab_subtile(tx, 0), slab_subtile(ty, 0), plyr_idx);
        return tag_blocks_for_digging_in_area(slab_subtile(tx, 0), slab_subtile(ty, 0), plyr_idx);
    }
    default:
        return false;
    }
}

void layer_vertical_dig_area_bounds(MapSlabCoord slb_x, MapSlabCoord slb_y, int size, MapSlabCoord *x0, MapSlabCoord *y0, MapSlabCoord *x1, MapSlabCoord *y1)
{
    size = max(1, min(size, LAYER_DIG_AREA_MAX));
    *x0 = slb_x - (size - 1) / 2;
    *y0 = slb_y - (size - 1) / 2;
    *x1 = *x0 + size - 1;
    *y1 = *y0 + size - 1;
}

int layer_vertical_dig_area_mode(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y, TbBool up, int size)
{
    // Untag if the tile in the middle was tagged; with nothing there, tag if anything in the area can be.
    const int state = layer_vertical_dig_tag_state(plyr_idx, slb_x, slb_y, up);
    if (state >= 0)
        return (state > 0) ? LVDigM_Untag : LVDigM_Tag;
    MapSlabCoord x0, y0, x1, y1;
    layer_vertical_dig_area_bounds(slb_x, slb_y, size, &x0, &y0, &x1, &y1);
    TbBool any = false;
    for (MapSlabCoord y = y0; y <= y1; y++)
    {
        for (MapSlabCoord x = x0; x <= x1; x++)
        {
            const int st = layer_vertical_dig_tag_state(plyr_idx, x, y, up);
            if (st == 0)
                return LVDigM_Tag;
            if (st > 0)
                any = true;
        }
    }
    return any ? LVDigM_Untag : -1;
}

TbBool layer_vertical_dig_area_would_change(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y, TbBool up, int size, int mode)
{
    MapSlabCoord x0, y0, x1, y1;
    layer_vertical_dig_area_bounds(slb_x, slb_y, size, &x0, &y0, &x1, &y1);
    for (MapSlabCoord y = y0; y <= y1; y++)
    {
        for (MapSlabCoord x = x0; x <= x1; x++)
        {
            const int st = layer_vertical_dig_tag_state(plyr_idx, x, y, up);
            if ((st == 0) && (mode == LVDigM_Tag))
                return true;
            if ((st > 0) && (mode == LVDigM_Untag))
                return true;
        }
    }
    return false;
}

TbBool layer_set_vertical_dig_area(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y, TbBool up, int size, int mode)
{
    if (mode == LVDigM_Toggle)
    {
        mode = layer_vertical_dig_area_mode(plyr_idx, slb_x, slb_y, up, size);
        if (mode < 0)
            return false;
    }
    MapSlabCoord x0, y0, x1, y1;
    layer_vertical_dig_area_bounds(slb_x, slb_y, size, &x0, &y0, &x1, &y1);
    TbBool changed = false;
    for (MapSlabCoord y = y0; y <= y1; y++)
    {
        for (MapSlabCoord x = x0; x <= x1; x++)
        {
            if (slab_coords_invalid(x, y))
                continue;
            if (layer_set_vertical_dig(plyr_idx, x, y, up, mode))
                changed = true;
        }
    }
    return changed;
}

int layer_vertical_dig_cursor_colour(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y, TbBool up)
{
    MapSlabCoord tx = 0;
    MapSlabCoord ty = 0;
    switch (resolve_vertical_dig(plyr_idx, slb_x, slb_y, up, &tx, &ty))
    {
    case LVDig_Floor:
        return (find_dig_from_task_list(plyr_idx, layer_floor_task_stl(tx, ty)) >= 0) ? SLC_YELLOW : SLC_GREEN;
    case LVDig_Wall:
        return (find_from_task_list_by_slab(plyr_idx, tx, ty) != -1) ? SLC_YELLOW : SLC_GREEN;
    default:
        return -1;
    }
}

TbBool layer_first_person_dig_possible(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y, TbBool up)
{
    MapSlabCoord tx = 0;
    MapSlabCoord ty = 0;
    return (resolve_vertical_dig(plyr_idx, slb_x, slb_y, up, &tx, &ty) != LVDig_None);
}

TbBool layer_first_person_dig(struct Thing *creatng, MapSlabCoord slb_x, MapSlabCoord slb_y, TbBool up)
{
    MapSlabCoord tx = 0;
    MapSlabCoord ty = 0;
    const int target = resolve_vertical_dig(creatng->owner, slb_x, slb_y, up, &tx, &ty);
    if (target == LVDig_None)
        return false;
    struct ShotConfigStats *shotst = get_shot_model_stats(ShM_Dig);
    thing_play_sample(creatng, shotst->dig.sndsample_idx + SOUND_RANDOM(shotst->dig.sndsample_range), NORMAL_PITCH, 0, 3, 0, 2, FULL_LOUDNESS);
    create_effect(&creatng->mappos, shotst->dig.effect_model, creatng->owner);
    if (target == LVDig_Floor)
    {
        if (layer_dig_floor_hit(creatng, layer_floor_task_stl(tx, ty)))
            thing_play_sample(creatng, snd_dig_impact + SOUND_RANDOM(snd_dig_impact_count), NORMAL_PITCH, 0, 3, 0, 4, FULL_LOUDNESS);
        return true;
    }
    // The wall on the other side of a dug floor, dug like any wall.
    struct SlabMap *slb = get_slabmap_block(tx, ty);
    if (slab_kind_is_indestructible(slb->kind))
        return true;
    const long damage = calculate_damage_did_to_slab_with_single_hit(creatng, slb);
    if (slb->health > damage)
    {
        slb->health -= damage;
        return true;
    }
    const MapSubtlCoord stl_x = slab_subtile_center(tx);
    const MapSubtlCoord stl_y = slab_subtile_center(ty);
    if ((get_slab_stats(slb)->block_flags & SlbAtFlg_Valuable) != 0)
        mine_out_block(stl_x, stl_y, creatng->owner);
    else
        dig_out_block(stl_x, stl_y, creatng->owner);
    check_map_explored(creatng, stl_x, stl_y);
    thing_play_sample(creatng, snd_dig_impact + SOUND_RANDOM(snd_dig_impact_count), NORMAL_PITCH, 0, 3, 0, 4, FULL_LOUDNESS);
    return true;
}

/******************************************************************************/
// Where imps stand to dig through a floor.

static int add_dig_position(PlayerNumber plyr_idx, MapSubtlCoord stl_x, MapSubtlCoord stl_y,
    MapSubtlCoord *stl_xs, MapSubtlCoord *stl_ys, int count, int max_count)
{
    if (count >= max_count)
        return count;
    if ((stl_x < 0) || (stl_y < 0) || (stl_x >= game.map_subtiles_x) || (stl_y >= game.map_subtiles_y))
        return count;
    if (!valid_dig_position(plyr_idx, stl_x, stl_y) || subtile_is_layer_pit(stl_x, stl_y))
        return count;
    for (int i = 0; i < count; i++)
    {
        if ((stl_xs[i] == stl_x) && (stl_ys[i] == stl_y))
            return count;
    }
    stl_xs[count] = stl_x;
    stl_ys[count] = stl_y;
    return count + 1;
}

/** Beside the tile, on the edge subtiles of its four neighbours, on the tile's own layer. */
static int dig_positions_beside(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y,
    MapSubtlCoord *stl_xs, MapSubtlCoord *stl_ys, int count, int max_count)
{
    for (int i = 0; i < SMALL_AROUND_LENGTH; i++)
    {
        const int dx = small_around[i].delta_x;
        const int dy = small_around[i].delta_y;
        if (!slab_is_open_tile(slb_x + dx, slb_y + dy))
            continue;
        const MapSubtlCoord cx = slab_subtile_center(slb_x) + 2 * dx;
        const MapSubtlCoord cy = slab_subtile_center(slb_y) + 2 * dy;
        count = add_dig_position(plyr_idx, cx, cy, stl_xs, stl_ys, count, max_count);
        count = add_dig_position(plyr_idx, cx + dy, cy + dx, stl_xs, stl_ys, count, max_count);
        count = add_dig_position(plyr_idx, cx - dy, cy - dx, stl_xs, stl_ys, count, max_count);
    }
    return count;
}

/** On the tile itself (under the floor to dig), middle first. */
static int dig_positions_on(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y,
    MapSubtlCoord *stl_xs, MapSubtlCoord *stl_ys, int count, int max_count)
{
    if (!slab_is_open_tile(slb_x, slb_y))
        return count;
    const MapSubtlCoord cx = slab_subtile_center(slb_x);
    const MapSubtlCoord cy = slab_subtile_center(slb_y);
    count = add_dig_position(plyr_idx, cx, cy, stl_xs, stl_ys, count, max_count);
    for (int i = 0; i < SMALL_AROUND_LENGTH; i++)
        count = add_dig_position(plyr_idx, cx + small_around[i].delta_x, cy + small_around[i].delta_y, stl_xs, stl_ys, count, max_count);
    return count;
}

int layer_vertical_dig_positions(PlayerNumber plyr_idx, unsigned char task_kind, SubtlCodedCoords task_stl,
    MapSubtlCoord *stl_xs, MapSubtlCoord *stl_ys, int max_count)
{
    if (!map_has_layers())
        return 0;
    const MapSlabCoord slb_x = subtile_slab(stl_num_decode_x(task_stl));
    const MapSlabCoord slb_y = subtile_slab(stl_num_decode_y(task_stl));
    MapSlabCoord bx;
    MapSlabCoord by;
    const TbBool has_below = slab_on_other_layer(slb_x, slb_y, -1, &bx, &by);
    int count = 0;
    if (task_kind == SDDigTask_DigLayerFloor)
    {
        // From above, beside the tile; from below, standing under it.
        if (slab_is_open_tile(slb_x, slb_y))
            count = dig_positions_beside(plyr_idx, slb_x, slb_y, stl_xs, stl_ys, count, max_count);
        if (has_below)
            count = dig_positions_on(plyr_idx, bx, by, stl_xs, stl_ys, count, max_count);
        return count;
    }
    // A wall: from under it once its floor is gone, or from above around the hole over it.
    if (has_below && layer_floor_is_dug(slb_x, slb_y))
        count = dig_positions_on(plyr_idx, bx, by, stl_xs, stl_ys, count, max_count);
    MapSlabCoord ax;
    MapSlabCoord ay;
    if (slab_on_other_layer(slb_x, slb_y, 1, &ax, &ay) && layer_floor_is_dug(ax, ay) && slab_is_open_tile(ax, ay))
        count = dig_positions_beside(plyr_idx, ax, ay, stl_xs, stl_ys, count, max_count);
    return count;
}

TbBool layer_dig_task_has_vertical_access(PlayerNumber plyr_idx, unsigned char task_kind, SubtlCodedCoords task_stl)
{
    MapSubtlCoord stl_x;
    MapSubtlCoord stl_y;
    return (layer_vertical_dig_positions(plyr_idx, task_kind, task_stl, &stl_x, &stl_y, 1) > 0);
}

/******************************************************************************/

static void remove_floor_task_for_all_players(MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    const SubtlCodedCoords stl_num = layer_floor_task_stl(slb_x, slb_y);
    for (PlayerNumber plyr_idx = 0; plyr_idx < PLAYERS_COUNT; plyr_idx++)
    {
        if (!player_is_keeper(plyr_idx))
            continue;
        const long task_idx = find_dig_from_task_list(plyr_idx, stl_num);
        if (task_idx >= 0)
            remove_from_task_list(plyr_idx, task_idx);
        show_layer_floor_tag(plyr_idx, slb_x, slb_y, false);
    }
}

static void dig_out_layer_floor(MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    const long slb_num = get_slab_number(slb_x, slb_y);
    game.layer_floor_dug[slb_num] = 1;
    game.layer_floor_damage[slb_num] = 0;
    remove_floor_task_for_all_players(slb_x, slb_y);
    layer_floor_changed(slb_x, slb_y);
    SYNCDBG(7, "The floor under slab (%d,%d) was dug out", (int)slb_x, (int)slb_y);
}

TbBool layer_dig_out_floor_now(MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    if ((get_layer_at_slab(slb_x, slb_y) <= 0) || layer_floor_is_dug(slb_x, slb_y))
        return false;
    dig_out_layer_floor(slb_x, slb_y);
    return true;
}

TbBool layer_dig_floor_hit(struct Thing *diggertng, SubtlCodedCoords task_stl)
{
    const MapSlabCoord slb_x = subtile_slab(stl_num_decode_x(task_stl));
    const MapSlabCoord slb_y = subtile_slab(stl_num_decode_y(task_stl));
    if (layer_floor_is_dug(slb_x, slb_y))
    {
        remove_floor_task_for_all_players(slb_x, slb_y);
        return true;
    }
    if (get_layer_at_slab(slb_x, slb_y) <= 0)
        return true;
    // A floor is as hard as a block of earth.
    const long health = game.block_health[get_slab_kind_stats(SlbT_EARTH)->block_health_index];
    const long damage = game.conf.rules[diggertng->owner].workers.default_imp_dig_damage;
    const long slb_num = get_slab_number(slb_x, slb_y);
    long total = (long)game.layer_floor_damage[slb_num] + max(damage, 1L);
    if (total < health)
    {
        game.layer_floor_damage[slb_num] = (uint16_t)min(total, 65535L);
        return false;
    }
    dig_out_layer_floor(slb_x, slb_y);
    return true;
}

void layer_dig_face_position(const struct Thing *diggertng, struct Coord3d *pos)
{
    if (!map_has_layers())
        return;
    const int from = get_layer_at_coord(pos->x.val, pos->y.val);
    const int to = get_layer_at_coord(diggertng->mappos.x.val, diggertng->mappos.y.val);
    if ((from < 0) || (to < 0) || (from == to))
        return;
    MapCoord x = pos->x.val;
    MapCoord y = pos->y.val;
    translate_coord_to_layer(from, to, &x, &y);
    pos->x.val = x;
    pos->y.val = y;
}


/******************************************************************************/
// Blue tag pattern: a recoloured copy of the usual (yellow) one, made from its current frame.

static unsigned char layer_blue_tag_lut[256];
static TbBool layer_blue_tag_lut_ready = false;
static unsigned char *layer_blue_tag_pixels = NULL;

static void build_layer_blue_tag_lut(void)
{
    if (engine_palette == NULL)
        return;
    for (int c = 0; c < 256; c++)
    {
        const int r = engine_palette[3 * c];
        const int g = engine_palette[3 * c + 1];
        const int bl = engine_palette[3 * c + 2];
        const int lum = (2 * r + 5 * g + bl) / 8;
        // The same brightness, in blue.
        const int tr = lum * 35 / 100;
        const int tg = lum * 60 / 100;
        const int tb = min(63, lum * 120 / 100 + 6);
        int best = c;
        long best_dist = LONG_MAX;
        for (int p = 1; p < 256; p++)
        {
            const long dr = engine_palette[3 * p] - tr;
            const long dg = engine_palette[3 * p + 1] - tg;
            const long db = engine_palette[3 * p + 2] - tb;
            const long dist = dr * dr + dg * dg + db * db;
            if (dist < best_dist)
            {
                best_dist = dist;
                best = p;
            }
        }
        layer_blue_tag_lut[c] = (unsigned char)best;
    }
    layer_blue_tag_lut_ready = true;
}

void layer_dig_update_tag_textures(void)
{
    if (!layer_blue_tag_lut_ready)
        build_layer_blue_tag_lut();
    if (!layer_blue_tag_lut_ready || (block_dimension <= 0) || (block_count_per_row <= 0))
        return;
    // Tiles are read with the stride of the texture sheet they come from, so the copy uses it too.
    const long stride = block_count_per_row * block_dimension;
    const long tile_bytes = stride * block_dimension;
    if (layer_blue_tag_pixels == NULL)
    {
        layer_blue_tag_pixels = (unsigned char *)calloc((size_t)TEXTURE_VARIATIONS_COUNT, (size_t)tile_bytes);
        if (layer_blue_tag_pixels == NULL)
            return;
    }
    for (int v = 0; v < TEXTURE_VARIATIONS_COUNT; v++)
    {
        const unsigned char *src = block_ptrs[v * TEXTURE_BLOCKS_COUNT + TEXTURE_LAND_MARKED_LAND];
        if (src == NULL)
            continue;
        unsigned char *dst = layer_blue_tag_pixels + v * tile_bytes;
        for (long y = 0; y < block_dimension; y++)
            for (long x = 0; x < block_dimension; x++)
                dst[y * stride + x] = layer_blue_tag_lut[src[y * stride + x]];
        block_ptrs[v * TEXTURE_BLOCKS_COUNT + LAYER_TAG_UP_TEXTURE] = dst;
    }
}

unsigned short layer_marked_land_texture(MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if (!map_has_layers() || (layer_blue_tag_pixels == NULL))
        return TEXTURE_LAND_MARKED_LAND;
    MapSubtlCoord ax = stl_x;
    MapSubtlCoord ay = stl_y;
    if (!get_subtile_above_layer(&ax, &ay))
        return TEXTURE_LAND_MARKED_LAND;
    if (find_dig_from_task_list(my_player_number, layer_floor_task_stl(subtile_slab(ax), subtile_slab(ay))) >= 0)
        return LAYER_TAG_UP_TEXTURE;
    return TEXTURE_LAND_MARKED_LAND;
}
/******************************************************************************/
#ifdef __cplusplus
}
#endif
