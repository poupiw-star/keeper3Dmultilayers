/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file map_layers.h
 *     Header file for map_layers.c.
 * @par Purpose:
 *     Dungeon layers: separate areas of one map that stand for the floors
 *     of a multi-level dungeon (Ground, +1, +2, +3).
 * @par Comment:
 *     Every layer has the same size, so local tile (x, y) on one layer sits
 *     directly above local tile (x, y) on the layer below.
 *     A map with no LAYER lines in its .lof file has one layer: the whole map.
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#ifndef DK_MAP_LAYERS_H
#define DK_MAP_LAYERS_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
#define MAX_MAP_LAYERS 4
#define MAP_LAYER_NAME_LEN 32

#pragma pack(1)

/** One layer: a rectangle of slabs (inclusive bounds) and its display name. */
struct MapLayer {
    int16_t x0;
    int16_t y0;
    int16_t x1;
    int16_t y1;
    char name[MAP_LAYER_NAME_LEN];
};

#pragma pack()
/******************************************************************************/
struct LevelInformation;

/** Parses the rest of a "LAYER = x0 y0 x1 y1 name" line into the level info. */
TbBool parse_level_layer_line(struct LevelInformation *lvinfo, const char *buf, int32_t *pos, long len, const char *fname);

/** Copies the layers of the level into the game state and checks them. */
void init_map_layers(LevelNumber lvnum);

/** Number of layers; a map without layers counts as one. */
int get_map_layers_count(void);
/** True when the map has two or more layers. */
TbBool map_has_layers(void);

/** Layer index at a slab, or -1 for rock between layers. */
int get_layer_at_slab(MapSlabCoord slb_x, MapSlabCoord slb_y);
/** Layer index at a subtile, or -1 for rock between layers. */
int get_layer_at_subtile(MapSubtlCoord stl_x, MapSubtlCoord stl_y);
/** Layer index at a map coordinate, or -1 for rock between layers. */
int get_layer_at_coord(MapCoord pos_x, MapCoord pos_y);

/** Display name of a layer, never NULL. */
const char *get_layer_name(int layer);

/** Moves a map coordinate to the same local position on another layer. */
TbBool translate_coord_to_layer(int from_layer, int to_layer, MapCoord *pos_x, MapCoord *pos_y);

/** Keeps a camera position on the layer the camera is on now (at from_x, from_y). */
void clamp_view_position_to_layer(MapCoord from_x, MapCoord from_y, MapCoord *pos_x, MapCoord *pos_y);

/******************************************************************************/
// Pits: a tile above Ground that is open, with the same tile on the layer below open too.
// Pits behave like abyss tiles: walkers avoid them, things fall in and land on the layer below.

/** Depth, in subtile heights, at which a pit shows the floor of the layer below. With the floor cube, a
 * storey is then as tall as the tallest column, so a layer starts right where the columns below end. */
#define LAYER_PIT_VIEW_DEPTH 7
/** Height, in cubes, of a standard floor; the bottom of a pit stands for that floor surface. */
#define LAYER_PIT_FLOOR_CUBES 1
/** Height of one storey, in cubes: z on a layer = z on the layer above + this many cubes. */
#define LAYER_STOREY_CUBES (LAYER_PIT_VIEW_DEPTH + LAYER_PIT_FLOOR_CUBES)
/** How many storeys down can be seen through stacked pits (every layer below the top one). */
#define LAYER_MAX_VIEW_STOREYS (MAX_MAP_LAYERS - 1)
/** Deepest point drawn down a pit, in cubes below this layer's floor level. */
#define LAYER_PIT_MAX_DEPTH (LAYER_MAX_VIEW_STOREYS * LAYER_STOREY_CUBES)

/** True if the slab is a pit: open, not a bridge, and open on the layer below. */
TbBool slab_is_layer_pit(MapSlabCoord slb_x, MapSlabCoord slb_y);
/** True if the subtile is part of a pit. */
TbBool subtile_is_layer_pit(MapSubtlCoord stl_x, MapSubtlCoord stl_y);
/** A pit things fall through: its floor is dug and the tile below is open. A dug floor over a wall
 * below is a pit for drawing only (it shows the wall's top), and can't be walked into. */
TbBool slab_is_layer_fall_pit(MapSlabCoord slb_x, MapSlabCoord slb_y);
TbBool subtile_is_layer_fall_pit(MapSubtlCoord stl_x, MapSubtlCoord stl_y);
/** The floor under a tile was dug out: pits, navigation and connectors are worked out again around it. */
void layer_floor_changed(MapSlabCoord slb_x, MapSlabCoord slb_y);
/** True if the subtile is directly under a pit on the layer above (its ceiling is open). */
TbBool subtile_is_below_layer_pit(MapSubtlCoord stl_x, MapSubtlCoord stl_y);
/** Distance, in slabs, from a pit within which the layer above is drawn when looking up from below. */
#define LAYER_ABOVE_VIEW_RADIUS 6
/** First-person view distance, in subtiles, on maps with layers (the game setting's lowest is 30). */
#define LAYER_FIRST_PERSON_VIEW_CELLS 60
/** Distance, in slabs, from a pit within which the layer below is drawn under the floor (seen at an angle). */
#define LAYER_BELOW_VIEW_RADIUS 2
/** Distance in slabs from a subtile to the nearest pit on its own layer (0 inside a pit, 255 if far). Cached per game turn. */
int subtile_layer_pit_distance(MapSubtlCoord stl_x, MapSubtlCoord stl_y);
/** True if a pit on this subtile's own layer is within LAYER_BELOW_VIEW_RADIUS slabs. */
TbBool subtile_near_layer_pit(MapSubtlCoord stl_x, MapSubtlCoord stl_y);
/** Gives the same subtile the given number of layers below; false if there is no such layer. */
TbBool get_subtile_layers_below(MapSubtlCoord *stl_x, MapSubtlCoord *stl_y, int storeys);
/** Gives the same subtile on the layer below; false if there is no layer below. */
TbBool get_subtile_below_layer(MapSubtlCoord *stl_x, MapSubtlCoord *stl_y);
/** True if a pit on the layer above is within LAYER_ABOVE_VIEW_RADIUS slabs of this subtile, so the layer above may be seen from here. */
TbBool subtile_near_layer_pit_above(MapSubtlCoord stl_x, MapSubtlCoord stl_y);
/** Gives the same subtile on the layer above; false if there is no layer above. */
TbBool get_subtile_above_layer(MapSubtlCoord *stl_x, MapSubtlCoord *stl_y);
/** For a pit subtile, gives the same subtile on the layer below. */
TbBool get_subtile_below_layer_pit(MapSubtlCoord *stl_x, MapSubtlCoord *stl_y);
/** Refreshes pathing on the layer above a slab whose kind changed, since pits there may open or close. */
void update_layer_above_slab(MapSlabCoord slb_x, MapSlabCoord slb_y);

/** Extra sound distance per storey between the listener's layer and a sound's layer (4 cubes). */
#define LAYER_SOUND_STOREY_DISTANCE (4 * COORD_PER_STL)
/** Sound hook: hears an emitter on another layer as if the layers were stacked; returns 1 if it was moved. */
long layer_sound_position_map(long recv_x, long recv_y, long recv_z, long *emit_x, long *emit_y, long *emit_z);

struct Thing;
/** Moves a thing that fell into a pit to the same spot on the layer below, near its ceiling. */
TbBool drop_thing_through_layer_pit(struct Thing *thing);

/******************************************************************************/
#ifdef __cplusplus
}
#endif
#endif
