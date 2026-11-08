/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file map_layer_dig.h
 *     Header file for map_layer_dig.c.
 * @par Purpose:
 *     Dungeon layers: digging through the floor between two layers, from above
 *     or from below, and digging walls through such a hole.
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#ifndef DK_MAP_LAYER_DIG_H
#define DK_MAP_LAYER_DIG_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
struct Thing;
struct Coord3d;

/** In first person, how far (in angle units, of 2048 a turn) a digger must look down or up to dig a floor. */
#define LAYER_FIRST_PERSON_DIG_PITCH 100

/** Spare animated texture slot holding a blue copy of the dig tag pattern, for ceilings tagged to dig up. */
#define LAYER_TAG_UP_TEXTURE 999

/** The tag pattern for a tagged floor subtile: blue under a floor tagged to dig through (seen from below),
 * the usual yellow otherwise. */
unsigned short layer_marked_land_texture(MapSubtlCoord stl_x, MapSubtlCoord stl_y);
/** Remakes the blue tag pattern from the current frame of the usual one; called when animated textures step. */
void layer_dig_update_tag_textures(void);

/** Clears every floor between layers back to solid, for a new level. */
void clear_layer_floors(void);
/** True if the floor under this tile (on a layer above Ground) has been dug out. */
TbBool layer_floor_is_dug(MapSlabCoord slb_x, MapSlabCoord slb_y);
/** Digs out the floor under a tile at once (level scripts); false if there is none to dig. */
TbBool layer_dig_out_floor_now(MapSlabCoord slb_x, MapSlabCoord slb_y);

/**
 * A player tags or untags, from the tile clicked, the next solid piece below it (up = false) or above
 * it (up = true): the floor between the layers first, then the wall behind that floor.
 * @return true if something was tagged or untagged.
 */
TbBool layer_toggle_vertical_dig(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y, TbBool up);
/** What layer_set_vertical_dig() does to the piece it finds: toggle it, or only tag / only untag (drags, areas). */
enum LayerVerticalDigMode {
    LVDigM_Toggle = 0,
    LVDigM_Tag,
    LVDigM_Untag,
};
/** Widest square area tagged at once, in tiles. */
#define LAYER_DIG_AREA_MAX 15
/** As layer_toggle_vertical_dig(), with a LVDigM_* mode. */
TbBool layer_set_vertical_dig(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y, TbBool up, int mode);
/** The same for every tile of a square area (size tiles wide) around a tile, as wall digging does with
 * its room-sized area. In LVDigM_Toggle the area untags if its middle was tagged, else it tags. */
TbBool layer_set_vertical_dig_area(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y, TbBool up, int size, int mode);
/** The square area around a tile, as tagged by layer_set_vertical_dig_area(). */
void layer_vertical_dig_area_bounds(MapSlabCoord slb_x, MapSlabCoord slb_y, int size, MapSlabCoord *x0, MapSlabCoord *y0, MapSlabCoord *x1, MapSlabCoord *y1);
/** What a click on an area would do: LVDigM_Tag or LVDigM_Untag, or -1 if there is nothing to tag there. */
int layer_vertical_dig_area_mode(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y, TbBool up, int size);
/** True if tagging (or untagging) the area would change anything. */
TbBool layer_vertical_dig_area_would_change(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y, TbBool up, int size, int mode);
/** Whether what a click there would hit is tagged: 1 tagged, 0 not tagged, -1 nothing to tag there. */
int layer_vertical_dig_tag_state(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y, TbBool up);
/** For the client: whether a click there could tag something through a floor (same rules, no change). */
TbBool layer_vertical_dig_click_possible(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y, TbBool up);

/** Colour of the cursor box over a tile for a click there (SLC_*): green to tag, yellow to untag; -1 if nothing to tag. */
int layer_vertical_dig_cursor_colour(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y, TbBool up);
/**
 * A creature in first person digs through a floor: down into the floor of the tile slb (or the wall under
 * it once the floor is gone), or up into the ceiling over it (or the wall above it).
 * @return true if it hit something there.
 */
TbBool layer_first_person_dig(struct Thing *creatng, MapSlabCoord slb_x, MapSlabCoord slb_y, TbBool up);
/** Whether layer_first_person_dig() there would hit something (no change made). */
TbBool layer_first_person_dig_possible(PlayerNumber plyr_idx, MapSlabCoord slb_x, MapSlabCoord slb_y, TbBool up);

/** True if a dig task can be worked from another layer (through a floor, or a floor task itself). */
TbBool layer_dig_task_has_vertical_access(PlayerNumber plyr_idx, unsigned char task_kind, SubtlCodedCoords task_stl);
/**
 * Places an imp could stand to work the dig task from another layer, best first. For a floor task these
 * are the only places; for a wall they come after the usual ones beside it.
 * @return how many were written.
 */
int layer_vertical_dig_positions(PlayerNumber plyr_idx, unsigned char task_kind, SubtlCodedCoords task_stl,
    MapSubtlCoord *stl_xs, MapSubtlCoord *stl_ys, int max_count);
/** One dig hit on a floor task; true when the floor broke (the task is then done). */
TbBool layer_dig_floor_hit(struct Thing *diggertng, SubtlCodedCoords task_stl);
/** The spot an imp faces to dig the task, moved onto the imp's own layer. */
void layer_dig_face_position(const struct Thing *diggertng, struct Coord3d *pos);

/******************************************************************************/
#ifdef __cplusplus
}
#endif
#endif
