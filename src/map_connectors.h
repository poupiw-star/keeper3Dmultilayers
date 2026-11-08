/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file map_connectors.h
 *     Header file for map_connectors.c.
 * @par Purpose:
 *     Dungeon layers: connectors (stairs, ladders, elevators) between two
 *     neighbouring layers, and routing creatures through them.
 * @par Comment:
 *     A connector is a pair of slabs at the same local tile on two layers
 *     next to each other. The list is not stored: it is worked out from the
 *     map (rooms and doors on it) whenever the map changes.
 * @par  Copying and copyrights:
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 */
/******************************************************************************/
#ifndef DK_MAP_CONNECTORS_H
#define DK_MAP_CONNECTORS_H

#include "bflib_basics.h"
#include "globals.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
#define MAX_LAYER_CONNECTORS 256
/** Ceiling over stairs steps, in cubes: high enough to climb level with the floor above. */
#define LAYER_STAIRS_CEILING_SUBTILES 15

enum LayerConnectorKinds {
    LCnK_None = 0,
    LCnK_Stairs,
    LCnK_Ladder,
    LCnK_Elevator,
    LCnK_Pit,      /**< An open pit: only fliers cross it, one connector per group of pit tiles. */
};

/** A connector between layer lower_layer and the layer above it, at the same local slab on both. */
struct LayerConnector {
    unsigned char kind;
    signed char lower_layer;
    PlayerNumber owner;
    MapSlabCoord lower_slb_x;
    MapSlabCoord lower_slb_y;
    MapSlabCoord upper_slb_x;
    MapSlabCoord upper_slb_y;
    /** Stairs only: the side the steps rise towards (LCnD_*). The top step and the landing above are on that side. */
    unsigned char dir;
};

enum LayerConnectorDirs {
    LCnD_North = 0,
    LCnD_East,
    LCnD_South,
    LCnD_West,
};

/** A creature's climb between layers (CreatureControl layer_climb). */
enum LayerClimbStates {
    LClS_None = 0,
    LClS_Up,     /**< Rising from the lower end; it moves to the layer above at the top. */
    LClS_Down,   /**< Moved to the lower end high up, coming down to its floor. */
    LClS_Rest,   /**< Just arrived; a creature in first person must step off before it can climb again. */
    LClS_FlyRestAbove, /**< A flier just came up through a pit; it must rise out of it before it can go back down. */
    LClS_FlyRestBelow, /**< A flier just came down through a pit; it must sink away from the hole before it can go back up. */
};

struct Thing;
struct Coord3d;

/** Marks the connector list as out of date; called when any slab changes. */
void layer_connectors_changed(void);
/** Number of connectors on the map now. */
int get_layer_connectors_count(void);
/** Connector by index, or NULL. */
const struct LayerConnector *get_layer_connector(int idx);
/** Index of the connector with an end on the given slab, or -1. */
int get_layer_connector_at_slab(MapSlabCoord slb_x, MapSlabCoord slb_y);
/** True if the creature may use the connector (its kind, and for ladders, the lock). */
TbBool layer_connector_usable_by(const struct LayerConnector *conn, const struct Thing *creatng);

/** Room kind that is a connector: stairs or elevator, and LCnK_None for other rooms. */
int room_kind_layer_connector_kind(RoomKind rkind);
/** Dungeon layers: the Stairs room is always there to build on maps with layers, whatever the level script says. */
void layer_make_stairs_room_available(void);
/** A connector room slab was built: build the matching end on the layer above (or below) if it can go there. */
void layer_connector_room_placed(PlayerNumber owner, RoomKind rkind, MapSlabCoord slb_x, MapSlabCoord slb_y);
/** A connector room changed owner: the matching ends on the layers above and below follow. */
struct Room;
void layer_connector_room_owner_changed(struct Room *room);

/**
 * For a creature going to a position on another layer: the connector end on its own layer to walk to next.
 * @return true if there is a way; false if the target can't be reached through connectors.
 */
TbBool layer_route_next_hop(const struct Thing *creatng, const struct Coord3d *target, struct Coord3d *hop);
/**
 * For a creature that can't walk all the way to a position on another layer (a tunneller that will dig there):
 * the end, on the target's layer, of a connector it can get to, the one closest to the target.
 * @return true if one was found.
 */
TbBool layer_route_entry_towards(const struct Thing *creatng, const struct Coord3d *target, struct Coord3d *entry);
/** True if one place can be walked to from the other, on its layer or through stairs and elevators to other layers. */
TbBool layer_points_connected(const struct Coord3d *from, const struct Coord3d *to);
/** For a creature on the steps of stairs: the floor at the foot of them, where a tunneller starts digging. */
TbBool layer_stairs_foot_under(const struct Thing *creatng, struct Coord3d *foot);
/** True if the position is the end, on the creature's layer, of a connector the creature can use. */
TbBool layer_route_hop_still_valid(const struct Thing *creatng, const struct Coord3d *hop);
/** True if the creature and the position are on different layers (both on some layer). */
TbBool creature_target_on_other_layer(const struct Thing *creatng, const struct Coord3d *pos);

/** Once a turn, before things move: brings the connector list and the stairs shapes up to date. */
void layer_connectors_process_turn(void);
/** Works out the connectors and the stairs shapes for a new map state, without touching navigation. */
void layer_connectors_init(void);
struct Column;
/** The column drawn and walked on for a stairs step subtile, or NULL. Reads the last worked out shapes only. */
struct Column *layer_stairs_column_at(MapSubtlCoord stl_x, MapSubtlCoord stl_y);
/** Walking height on stairs at a position: a smooth slope over the steps. False if the position isn't on steps. */
TbBool layer_stairs_ramp_floor(MapCoord pos_x, MapCoord pos_y, MapCoord *floor_z);
/** Floor height used for route finding on a stairs step subtile (one cube per step), or -1. */
long layer_stairs_nav_floor_subtiles(MapSubtlCoord stl_x, MapSubtlCoord stl_y);
/** On stairs steps: the lowest a walker may be to move onto the step there; -1 elsewhere. */
MapCoord layer_stairs_entry_height(MapSubtlCoord stl_x, MapSubtlCoord stl_y);
/** For a stairs step column, the floor height (in cubes) of the room under the steps; -1 for other columns. */
long layer_stairs_column_base_subtiles(const struct Column *col);
/** True for a column returned by layer_stairs_column_at(). */
TbBool column_is_layer_stairs_column(const struct Column *col);
/** True if the subtile is stairs or next to them: there is no ceiling there but real ceiling cubes, as creatures walk up high. */
TbBool subtile_next_to_layer_stairs(MapSubtlCoord stl_x, MapSubtlCoord stl_y);
/** For a creature on the top tile of stairs: takes out of its next move whatever would carry it past the end
 * of the top step, where the layer above begins (it crosses there instead, or slides along the edge). */
void layer_stairs_keep_on_top(const struct Thing *thing, struct Coord3d *nxpos);
/** True if the subtile is in the row just past the end of stairs' top step, on the lower layer. */
TbBool subtile_is_beyond_layer_stairs(MapSubtlCoord stl_x, MapSubtlCoord stl_y);
/** True if the subtile is in the last row of stairs steps, where the climb to the layer above starts. */
TbBool subtile_is_layer_stairs_top_row(MapSubtlCoord stl_x, MapSubtlCoord stl_y);
/** True if the subtile is the open well above stairs, on the layer above them. */
TbBool subtile_is_layer_stairwell(MapSubtlCoord stl_x, MapSubtlCoord stl_y);
/** True if any subtile of the slab is an open stairwell. */
TbBool slab_has_layer_stairwell(MapSlabCoord slb_x, MapSlabCoord slb_y);

/** Starts the creature's climb through the connector end it stands on; true if it started. */
TbBool creature_start_layer_climb(struct Thing *creatng);
/** True while the creature is climbing between layers (it doesn't walk then). */
TbBool creature_is_climbing_layers(const struct Thing *creatng);
/**
 * Once a turn for each creature, after its movement is decided: moves a climbing creature, and starts
 * a climb for a creature in first person that walks into a connector.
 * @return true if the creature is climbing, so its normal movement is skipped this turn.
 */
TbBool update_creature_layer_climb(struct Thing *creatng);

/******************************************************************************/
#ifdef __cplusplus
}
#endif
#endif
