/******************************************************************************/
// Free implementation of Bullfrog's Dungeon Keeper strategy game.
/******************************************************************************/
/** @file roomspace_input.c
 *     Turns local mouse input into dig tag packet actions, and shows those tags and the cursor box before the game state catches up.
 */
/******************************************************************************/
#include "pre_inc.h"
#include "observer.h"
#include "game_legacy.h"
#include "config_sounds.h"
#include "map_data.h"
#include "cursor_tag.h"
#include "engine_render.h"
#include "frontmenu_ingame_evnt.h"
#include "player_data.h"
#include "power_hand.h"
#include "slab_data.h"
#include "tasks_list.h"
#include "packets.h"
#include "net_exchange_gameplay.h"
#include "roomspace.h"
#include "roomspace_input.h"

#include "map_layer_dig.h"
#include "map_layers.h"
#include "bflib_datetm.h"
#include "bflib_vidraw.h"
#include "bflib_video.h"
#include "kjm_input.h"
#include "bflib_keybrd.h"
#include "keeperfx.hpp"
#include "post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif
/******************************************************************************/
static struct DigTagPrediction dig_tags;

static struct {
    MapSlabCoord drag_start_slb_x;
    MapSlabCoord drag_start_slb_y;
    MapSlabCoord previous_slb_x;
    MapSlabCoord previous_slb_y;
    TbBool cursor_button_down;
    TbBool cursor_locked;
    unsigned char cursor_state;
    TbBool untag_mode;
} dig_cursor;

static GameTurn cursor_release_turn;
static MapSlabCoord room_drag_start_slb_x;
static MapSlabCoord room_drag_start_slb_y;

void reset_dig_prediction(void)
{
    memset(&dig_tags, 0, sizeof(dig_tags));
    memset(&dig_cursor, 0, sizeof(dig_cursor));
    cursor_release_turn = 0;
    set_local_user_state_prediction(NULL);
}

static void reconcile_dig_predictions(struct DigTagPrediction *tags, GameTurn processed_turn)
{
    tags->task_count = get_players_dungeon(get_my_player())->task_count;
    for (int i = 0; i < tags->slab_count;) {
        SlabCodedCoords slb_num = tags->slabs[i];
        if ((GameTurnDelta)(tags->slab_turns[slb_num] - processed_turn) <= 0) {
            tags->slab_tag_modes[slb_num] = 0;
            tags->slabs[i] = tags->slabs[--tags->slab_count];
            continue;
        }
        TbBool tagged = find_from_task_list_by_slab(my_player_number, slb_num_decode_x(slb_num), slb_num_decode_y(slb_num)) != -1;
        tags->task_count += (tags->slab_tag_modes[slb_num] == DigTagMode_Tag) - tagged;
        i++;
    }
}

static TbBool dig_input_is_blocked(const struct Packet *pckt)
{
    if (pckt == NULL) {
        return true;
    }
    if ((pckt->control_flags & (PCtr_Gui | PCtr_MapCoordsValid)) != PCtr_MapCoordsValid) {
        return true;
    }
    switch (pckt->action)
    {
    case PckA_UsePwrHandPick:
    case PckA_UsePwrOnThing:
    case PckA_BookmarkLoad: // Dragging minimap
        return true;
    }
    if (battle_creature_over > 0) {
        return true;
    }
    return false;
}

static TbBool dig_input_is_enabled(void)
{
    return !replay.load_enable && (get_my_player() != &local_observer_player);
}

TbBool local_cursor_is_locked(void)
{
    if (dig_cursor.cursor_button_down && dig_cursor.cursor_locked) {
        return true;
    }
    if (get_gameturn() - cursor_release_turn <= game.input_lag_turns) {
        return false;
    }
    return get_local_user_state()->one_click_lock_cursor;
}

/** Dungeon layers: Alt held turns hand clicks on open floor into digging through floors. */
static TbBool layer_dig_key_held(void)
{
    return ((key_modifiers & KMod_ALT) != 0) || lbKeyOn[KC_LALT] || lbKeyOn[KC_RALT];
}

/** Dungeon layers: the width of the square area floor and ceiling tags cover: the same as wall digging's. */
static int layer_dig_area_size(void)
{
    const struct UserState *ustate = get_local_user_state_prediction();
    // Only a size picked by hand: the automatic room sizing changes it all the time.
    if (ustate->roomspace_mode != box_placement_mode)
        return 1;
    return max(1, min(ustate->roomspace_width, LAYER_DIG_AREA_MAX));
}

static void send_layer_vertical_dig(struct Packet *pckt, MapSlabCoord slb_x, MapSlabCoord slb_y, TbBool up, int size, int mode)
{
    set_packet_action(pckt, PckA_LayerVerticalDig, slb_x, slb_y, (up ? 1 : 0) | (size << 1), mode);
    play_non_3d_sample(snd_tile_dig);
}

/** Dungeon layers: a floor/ceiling dig drag in progress (local input only). */
static struct {
    TbBool active;
    TbBool up;          /**< Right button: digging up through ceilings. */
    int mode;           /**< LVDigM_Tag or LVDigM_Untag, from where the drag started. */
    int size;           /**< Width of the square area tagged around each tile crossed. */
    MapSlabCoord last_x; /**< The last tile the drag has dealt with. */
    MapSlabCoord last_y;
} layer_dig_drag;

/** Dungeon layers: while a floor/ceiling dig drag is held, tags (or untags) the area around each tile the
 * cursor crosses, walking from the last tile towards the cursor one tile at a time so a fast drag skips
 * none. Sends at most one packet a turn. True while the drag owns the mouse. */
static TbBool process_layer_vertical_dig_drag(struct Packet *pckt, MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    if (!layer_dig_drag.active)
        return false;
    const unsigned long held = layer_dig_drag.up ? PCtr_RBtnHeld : PCtr_LBtnHeld;
    const unsigned long click = layer_dig_drag.up ? PCtr_RBtnClick : PCtr_LBtnClick;
    if (((pckt->control_flags & (held | click)) == 0) || !map_has_layers())
    {
        layer_dig_drag.active = false;
        return false;
    }
    unset_packet_control(pckt, held | click);
    if (((pckt->control_flags & (PCtr_Gui | PCtr_MapCoordsValid)) != PCtr_MapCoordsValid) || !layer_dig_key_held())
        return true;
    if ((pckt->action != PckA_None) && (pckt->action != PckA_SetRoomspaceHighlight))
        return true;
    // Walk towards the cursor; tiles where nothing would change cost no packet, so several can pass in one go.
    for (int steps = 0; steps < 2 * MAX_TILES_X; steps++)
    {
        if ((layer_dig_drag.last_x == slb_x) && (layer_dig_drag.last_y == slb_y))
            break;
        layer_dig_drag.last_x += (slb_x > layer_dig_drag.last_x) ? 1 : ((slb_x < layer_dig_drag.last_x) ? -1 : 0);
        layer_dig_drag.last_y += (slb_y > layer_dig_drag.last_y) ? 1 : ((slb_y < layer_dig_drag.last_y) ? -1 : 0);
        if (!layer_vertical_dig_area_would_change(my_player_number, layer_dig_drag.last_x, layer_dig_drag.last_y,
                layer_dig_drag.up, layer_dig_drag.size, layer_dig_drag.mode))
            continue;
        send_layer_vertical_dig(pckt, layer_dig_drag.last_x, layer_dig_drag.last_y, layer_dig_drag.up, layer_dig_drag.size, layer_dig_drag.mode);
        break;
    }
    return true;
}

/** Dungeon layers: with Alt held and an empty hand, a left click on open floor tags the floor under it
 * (dig down) and a right click tags the ceiling over it (dig up); clicking a tagged one untags it.
 * Like wall digging, the click covers the square area picked with the room size keys, and holding the
 * button and dragging does the same along the way. Once a floor is gone, the same click tags the wall on
 * the other side. True if the click was used. */
static TbBool process_layer_vertical_dig_click(struct Packet *pckt, MapSlabCoord slb_x, MapSlabCoord slb_y)
{
    if (process_layer_vertical_dig_drag(pckt, slb_x, slb_y))
        return true;
    if (!map_has_layers() || !layer_dig_key_held())
        return false;
    if ((pckt->control_flags & (PCtr_Gui | PCtr_MapCoordsValid)) != PCtr_MapCoordsValid)
        return false;
    const TbBool left = ((pckt->control_flags & PCtr_LBtnClick) != 0);
    const TbBool right = ((pckt->control_flags & PCtr_RBtnClick) != 0);
    if (left == right)
        return false;
    if ((pckt->action != PckA_None) && (pckt->action != PckA_SetRoomspaceHighlight))
        return false;
    if ((get_local_user_state_prediction()->work_state != PSt_CtrlDungeon) || !power_hand_is_empty(get_my_player())
        || (local_state.local_thing_under_hand != 0))
        return false;
    const TbBool up = right;
    const int size = layer_dig_area_size();
    const int mode = layer_vertical_dig_area_mode(my_player_number, slb_x, slb_y, up, size);
    if (mode < 0)
        return false;
    // Where the click lands decides what the whole drag does: untag if it was tagged, tag otherwise.
    layer_dig_drag.active = true;
    layer_dig_drag.up = up;
    layer_dig_drag.mode = mode;
    layer_dig_drag.size = size;
    layer_dig_drag.last_x = slb_x;
    layer_dig_drag.last_y = slb_y;
    send_layer_vertical_dig(pckt, slb_x, slb_y, up, size, mode);
    unset_packet_control(pckt, left ? (PCtr_LBtnClick | PCtr_LBtnHeld) : (PCtr_RBtnClick | PCtr_RBtnHeld));
    memset(&dig_cursor, 0, sizeof(dig_cursor));
    return true;
}

/** Dungeon layers: the tag box over an open floor tile that a click would tag through (down, or up with Alt). */
static void show_layer_vertical_dig_cursor(const struct Packet *pckt, MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    if (!map_has_layers() || ((pckt->control_flags & PCtr_MapCoordsValid) == 0))
        return;
    if (!power_hand_is_empty(get_my_player()) || (local_state.local_thing_under_hand != 0))
        return;
    const MapSlabCoord slb_x = subtile_slab(stl_x);
    const MapSlabCoord slb_y = subtile_slab(stl_y);
    if (!layer_dig_key_held())
        return;
    // Yellow: Alt+left click digs down there. Blue: Alt+right click digs up. Both possible: it alternates.
    const TbBool can_down = (layer_vertical_dig_cursor_colour(my_player_number, slb_x, slb_y, false) >= 0);
    const TbBool can_up = (layer_vertical_dig_cursor_colour(my_player_number, slb_x, slb_y, true) >= 0);
    if ((!can_down && !can_up) || !should_draw_cursor_box(get_local_user()))
        return;
    TbBool show_up = can_up;
    if (can_down && can_up)
        show_up = ((LbTimerClock() / 500) % 2) != 0;
    const int colour = show_up ? SLC_BLUE : SLC_YELLOW;
    map_volume_box.visible = 1;
    map_volume_box.color = colour;
    MapSlabCoord x0, y0, x1, y1;
    layer_vertical_dig_area_bounds(slb_x, slb_y, layer_dig_area_size(), &x0, &y0, &x1, &y1);
    map_volume_box.beg_x = subtile_coord(slab_subtile(x0, 0), 0);
    map_volume_box.beg_y = subtile_coord(slab_subtile(y0, 0), 0);
    map_volume_box.end_x = subtile_coord(slab_subtile(x1, 0) + STL_PER_SLB, 0);
    map_volume_box.end_y = subtile_coord(slab_subtile(y1, 0) + STL_PER_SLB, 0);
    map_volume_box.floor_height_z = floor_height_for_volume_box(my_player_number, slb_x, slb_y);
}

static TbBool update_dig_highlight(const struct Packet *pckt, MapSubtlCoord stl_x, MapSubtlCoord stl_y)
{
    struct UserState *ustate = get_local_user_state();
    struct PlayerInfo *player = get_my_player();
    unsigned char cursor_context = (unsigned char)((pckt->additional_packet_values & PCAdV_ContextMask) >> 1);
    TbBool cursor_is_locked = dig_cursor.cursor_button_down && dig_cursor.cursor_locked && ((pckt->control_flags & PCtr_LBtnClick) == 0) && ((pckt->control_flags & (PCtr_LBtnHeld | PCtr_LBtnRelease)) != 0);
    if ((cursor_context != CSt_PickAxe) && !cursor_is_locked) {
        if ((cursor_context != CSt_PowerHand) || power_hand_is_empty(player) || !can_dig_here(stl_x, stl_y, my_player_number, true)) {
            show_layer_vertical_dig_cursor(pckt, stl_x, stl_y);
            return false;
        }
    }
    if (!cursor_is_locked && (((pckt->control_flags & PCtr_LBtnAnyAction) == 0) || ((pckt->control_flags & PCtr_LBtnClick) != 0))) {
        ustate->swap_to_untag_mode = 0;
    }
    ustate->one_click_lock_cursor = cursor_is_locked;
    ustate->render_roomspace.drag_mode = cursor_is_locked;
    if (cursor_is_locked) {
        ustate->render_roomspace.drag_start_x = dig_cursor.drag_start_slb_x;
        ustate->render_roomspace.drag_start_y = dig_cursor.drag_start_slb_y;
        ustate->render_roomspace.untag_mode = dig_cursor.untag_mode;
    }
    get_dungeon_highlight_user_roomspace(&ustate->render_roomspace, player, get_local_user(), pckt, stl_x, stl_y, &dig_tags);
    tag_cursor_blocks_dig(player, get_local_user(), pckt, &ustate->render_roomspace, stl_x, stl_y, true);
    return true;
}

static void apply_packet_prediction(NetUserId user, const struct Packet *pckt)
{
    struct UserState *ustate = get_user_state(user);
    apply_roomspace_packet_action(user, pckt);
    apply_user_pref_packet_action(ustate, pckt);
    apply_first_person_packet_action(ustate, pckt);
    apply_creature_input_packet_values(ustate, pckt);
    if (ustate->view_type == PVT_DungeonTop) {
        process_dungeon_control_packet_spell_overcharge(user, pckt);
    }
    ustate->primary_cursor_state = (pckt->additional_packet_values & PCAdV_ContextMask) >> 1;
    if (pckt->action == PckA_SetPlyrState) {
        ustate->work_state = pckt->actn_par1;
        set_user_work_state_kind(ustate, pckt->actn_par2);
    }
    if (pckt->action == PckA_PlyrQueryCreature) {
        ustate->influenced_thing_idx = pckt->actn_par1;
    }
}

/**
 * Runs the singleplayer roomspace cursor code for a local packet, on the user state as it will be once the pending local packets are processed.
 * pending_turns counts the pending packets before this one; it is negative when this packet has been processed already.
 */
static TbBool get_predicted_roomspace(const struct Packet *pckt, GameTurnDelta pending_turns, TbBool include_drop_preview, struct UserState *predicted_ustate)
{
    NetUserId user = get_local_user();
    struct UserState *ustate = get_local_user_state();
    struct Packet *direct_packet = get_local_packet();
    struct UserState saved_ustate = *ustate;
    struct Packet saved_packet = *direct_packet;
    for (GameTurnDelta i = pending_turns; i > 0; i--) {
        const struct Packet *pending_pckt = get_history_packet(user, get_gameturn() - i);
        if (pending_pckt != NULL) {
            apply_packet_prediction(user, pending_pckt);
        }
    }
    TbBool has_roomspace = !dig_input_is_blocked(pckt);
    if ((pckt != NULL) && (pending_turns >= 0)) {
        apply_packet_prediction(user, pckt);
    }
    if (has_roomspace) {
        *direct_packet = *pckt;
        MapSubtlCoord stl_x = coord_subtile(pckt->pos_x);
        MapSubtlCoord stl_y = coord_subtile(pckt->pos_y);
        ustate->render_roomspace.drag_start_x = room_drag_start_slb_x;
        ustate->render_roomspace.drag_start_y = room_drag_start_slb_y;
        if (ustate->work_state == PSt_BuildRoom) {
            update_dungeon_build_roomspace_preview(user, stl_x, stl_y);
        } else if (ustate->work_state == PSt_Sell) {
            update_dungeon_sell_roomspace_preview(user, stl_x, stl_y);
        } else if (ustate->work_state != PSt_CtrlDungeon) {
            tag_cursor_blocks_for_work_state(user, stl_x, stl_y);
        } else {
            has_roomspace = update_dig_highlight(pckt, stl_x, stl_y);
            TbBool holding_thing = !power_hand_is_empty(get_my_player());
            TbBool chosen_subtile_is_high = has_roomspace && holding_thing;
            set_flag_value(ustate->additional_flags, UsrAF_ChosenSubTileIsHigh, chosen_subtile_is_high);
            if (chosen_subtile_is_high) {
                ustate->thing_under_hand = 0;
            }
            if (!has_roomspace && include_drop_preview && holding_thing) {
                tag_cursor_blocks_thing_in_hand(user, stl_x, stl_y);
                has_roomspace = true;
            }
        }
    }
    *predicted_ustate = *ustate;
    *ustate = saved_ustate;
    *direct_packet = saved_packet;
    return has_roomspace;
}

static void apply_dig_tag_selection(struct Packet *pckt, struct UserState *predicted_ustate)
{
    struct RoomSpace *roomspace = &predicted_ustate->render_roomspace;
    dig_cursor.untag_mode = roomspace->untag_mode;
    TbBool apply_selection = (pckt->control_flags & PCtr_LBtnHeld) != 0;
    if (predicted_ustate->roomspace_highlight_mode == drag_placement_mode) {
        apply_selection = (pckt->control_flags & PCtr_LBtnRelease) != 0;
    }
    if (!apply_selection) {
        return;
    }
    dig_tags.turn = pckt->turn;
    int changed_slab_count = apply_roomspace_dig_tag_selection(my_player_number, roomspace, dig_cursor.previous_slb_x, dig_cursor.previous_slb_y, predicted_ustate->roomspace_highlight_mode, &dig_tags);
    if (changed_slab_count > 0) {
        uint16_t previous_slb = (uint16_t)dig_cursor.previous_slb_x | ((uint16_t)dig_cursor.previous_slb_y << 8);
        set_packet_action(pckt, PckA_ApplyRoomspaceDigTag, predicted_ustate->roomspace_highlight_mode, predicted_ustate->roomspace_width, previous_slb, roomspace->untag_mode);
        play_non_3d_sample(snd_tile_dig);
    }
}

void update_dig_tag_input(void)
{
    struct Packet *pckt = get_local_packet();
    if (!dig_input_is_enabled()) {
        reset_dig_prediction();
        return;
    }
    MapSlabCoord slb_x = coord_slab(pckt->pos_x);
    MapSlabCoord slb_y = coord_slab(pckt->pos_y);
    if (((pckt->control_flags & (PCtr_LBtnRelease | PCtr_RBtnRelease)) != 0) && ((pckt->control_flags & PCtr_HeldAnyButton) == 0)) {
        cursor_release_turn = get_gameturn();
    }
    if (process_layer_vertical_dig_click(pckt, slb_x, slb_y)) {
        return;
    }
    if ((pckt->control_flags & (PCtr_Gui | PCtr_MapCoordsValid | PCtr_HeldAnyButton | PCtr_LBtnRelease)) == PCtr_MapCoordsValid) {
        room_drag_start_slb_x = slb_x;
        room_drag_start_slb_y = slb_y;
    }
    if (((pckt->control_flags & PCtr_RBtnClick) != 0) && ((pckt->control_flags & PCtr_LBtnHeld) != 0)) {
        memset(&dig_cursor, 0, sizeof(dig_cursor));
        return;
    }
    if ((pckt->control_flags & PCtr_LBtnAnyAction) == 0) {
        dig_cursor.cursor_button_down = false;
        return;
    }
    TbBool ignore_packet = ((pckt->action != PckA_None) && (pckt->action != PckA_SetRoomspaceHighlight)) || (get_local_user_state_prediction()->work_state != PSt_CtrlDungeon) || dig_input_is_blocked(pckt);
    unsigned char cursor_context = (unsigned char)((pckt->additional_packet_values & PCAdV_ContextMask) >> 1);
    TbBool click = (pckt->control_flags & PCtr_LBtnClick) != 0;
    if (click) {
        dig_cursor.cursor_button_down = !ignore_packet;
        dig_cursor.cursor_locked = false;
        dig_cursor.cursor_state = cursor_context;
    }
    if (click || dig_input_is_blocked(get_history_packet(get_local_user(), pckt->turn - 1))) {
        dig_cursor.previous_slb_x = slb_x;
        dig_cursor.previous_slb_y = slb_y;
    }
    if (ignore_packet || !dig_cursor.cursor_button_down) {
        return;
    }
    struct Packet selection_pckt = *pckt;
    if (!dig_cursor.cursor_locked) {
        selection_pckt.control_flags |= PCtr_LBtnClick;
    }
    struct UserState predicted_ustate;
    TbBool has_roomspace = get_predicted_roomspace(&selection_pckt, game.input_lag_turns, false, &predicted_ustate);
    if (!dig_cursor.cursor_locked) {
        if (click && !has_roomspace && (cursor_context == CSt_PowerHand) && (local_state.local_thing_under_hand != 0)) {
            dig_cursor.cursor_button_down = false;
            return;
        }
        dig_cursor.drag_start_slb_x = slb_x;
        dig_cursor.drag_start_slb_y = slb_y;
        dig_cursor.cursor_locked = has_roomspace && (cursor_context == dig_cursor.cursor_state);
    }
    if (dig_cursor.cursor_locked) {
        apply_dig_tag_selection(pckt, &predicted_ustate);
    }
    dig_cursor.previous_slb_x = slb_x;
    dig_cursor.previous_slb_y = slb_y;
}

unsigned char get_dig_prediction_render_flags(MapSubtlCoord stl_x, MapSubtlCoord stl_y, unsigned char base_map_flags)
{
    MapSlabCoord slb_x = subtile_slab(stl_x);
    MapSlabCoord slb_y = subtile_slab(stl_y);
    if (!dig_input_is_enabled() || slab_coords_invalid(slb_x, slb_y)) {
        return base_map_flags;
    }
    unsigned char predicted_dig_tag_mode = dig_tags.slab_tag_modes[get_slab_number(slb_x, slb_y)];
    if (predicted_dig_tag_mode == DigTagMode_Untag) {
        return base_map_flags & ~(SlbAtFlg_TaggedValuable | SlbAtFlg_Unexplored);
    }
    if (predicted_dig_tag_mode != DigTagMode_Tag) {
        return base_map_flags;
    }
    if ((base_map_flags & SlbAtFlg_Valuable) == 0) {
        return base_map_flags | SlbAtFlg_Unexplored;
    }
    if (!subtile_revealed(stl_x, stl_y, my_player_number)) {
        base_map_flags |= SlbAtFlg_Unexplored;
    }
    return base_map_flags | SlbAtFlg_TaggedValuable;
}

void update_dig_prediction_cursor_preview(void)
{
    set_local_user_state_prediction(NULL);
    if (!dig_input_is_enabled()) {
        return;
    }
    const struct Packet *pckt = get_history_packet(get_local_user(), get_gameturn());
    reconcile_dig_predictions(&dig_tags, get_local_packet()->turn);
    if (get_local_user_state()->work_state != PSt_OrderCreatr) {
        map_volume_box.visible = 0;
    }
    struct UserState predicted_ustate;
    get_predicted_roomspace(pckt, game.input_lag_turns - 1, true, &predicted_ustate);
    set_local_user_state_prediction(&predicted_ustate);
    if ((pckt != NULL) && ((pckt->control_flags & PCtr_LBtnRelease) != 0)) {
        dig_cursor.cursor_button_down = false;
    }
}
/******************************************************************************/
#ifdef __cplusplus
}
#endif
