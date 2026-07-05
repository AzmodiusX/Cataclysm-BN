#include "activity_actor.h"
#include "activity_actor_definitions.h"

#include <cmath>
#include <list>
#include <memory>
#include <string>
#include <utility>

#include "action_time_scale.h"
#include "activity_handlers.h" // put_into_vehicle_or_drop and drop_on_map
#include "activity_speed.h"
#include "advanced_inv.h"
#include "avatar.h"
#include "avatar_action.h"
#include "calendar.h"
#include "character.h"
#include "character_functions.h"
#include "construction.h"
#include "construction_partial.h"
#include "craft_command.h"
#include "crafting.h"
#include "debug.h"
#include "enums.h"
#include "event.h"
#include "event_bus.h"
#include "field_type.h"
#include "flag.h"
#include "game.h"
#include "gates.h"
#include "iexamine.h"
#include "int_id.h"
#include "item.h"
#include "item_group.h"
#include "item_hauling.h"
#include "json.h"
#include "line.h"
#include "locations.h"
#include "map.h"
#include "mapbuffer.h"
#include "map_iterator.h"
#include "map_selector.h"
#include "mapdata.h"
#include "messages.h"
#include "npc.h"
#include "options.h"
#include "pickup.h"
#include "player.h"
#include "player_activity.h"
#include "point.h"
#include "ranged.h"
#include "crafting_quality.h"
#include "recipe.h"
#include "recipe_dictionary.h"
#include "rng.h"
#include "sounds.h"
#include "timed_event.h"
#include "translations.h"
#include "uistate.h"
#include "vehicle.h"
#include "vehicle_part.h"
#include "vpart_position.h"

#define dbg(x) DebugLog((x),DC::Game)

static const construction_str_id deconstruct_simple( "constr_deconstruct_simple" );
static const construction_str_id deconstruct( "constr_deconstruct" );
static const construction_group_str_id
advanced_object_deconstruction( "advanced_object_deconstruction" );

static const itype_id itype_bone_human( "bone_human" );
static const itype_id itype_electrohack( "electrohack" );

static const skill_id skill_computer( "computer" );
static const skill_id skill_mechanics( "mechanics" );

static const mtype_id mon_zombie( "mon_zombie" );
static const mtype_id mon_zombie_fat( "mon_zombie_fat" );
static const mtype_id mon_zombie_rot( "mon_zombie_rot" );
static const mtype_id mon_skeleton( "mon_skeleton" );
static const mtype_id mon_zombie_crawler( "mon_zombie_crawler" );

static const quality_id qual_LOCKPICK( "LOCKPICK" );

static const trait_id trait_DEBUG_HS( "DEBUG_HS" );

static const std::string has_thievery_witness( "has_thievery_witness" );

int simple_task::to_counter() const
{
    double ret = 10'000'000.0 / moves_total * ( moves_total - moves_left );
    return std::round( ret );
}

inline void progress_counter::pop()
{
    if( empty() ) {
        dbg( DL::Error ) << "task was popped out of empty progress queue";
        return;
    }
    moves_left -= targets.front().moves_left;
    targets.pop_front();
    idx++;
}

inline void progress_counter::purge()
{
    if( empty() ) {
        dbg( DL::Error ) << "task was purged out of empty progress queue";
        return;
    }
    moves_left -= targets.front().moves_left;
    moves_total -= targets.front().moves_total;
    total_tasks--;
    targets.pop_front();
}

inline void activity_actor::calc_all_moves( player_activity &act, Character &who )
{
    act.speed.calc_all_moves( who );
}

aim_activity_actor::aim_activity_actor() : fake_weapon( new fake_item_location() )
{
    initial_view_offset = get_avatar().view_offset;
}

std::unique_ptr<aim_activity_actor> aim_activity_actor::use_wielded()
{
    return std::make_unique<aim_activity_actor>();
}

std::unique_ptr<aim_activity_actor> aim_activity_actor::use_bionic( detached_ptr<item> &&fake_gun,
        const units::energy &cost_per_shot )
{
    std::unique_ptr<aim_activity_actor> act( new aim_activity_actor() );
    act->bp_cost_per_shot = cost_per_shot;
    act->fake_weapon = std::move( fake_gun );
    return act;
}

std::unique_ptr<aim_activity_actor> aim_activity_actor::use_gear( item *gun )
{
    std::unique_ptr<aim_activity_actor> act( new aim_activity_actor() );
    act->weapon = safe_reference<item>( gun );
    return act;
}

std::unique_ptr<aim_activity_actor> aim_activity_actor::use_mutation( detached_ptr<item>
        &&fake_gun )
{
    std::unique_ptr<aim_activity_actor> act( new aim_activity_actor() );
    act->fake_weapon = std::move( fake_gun );
    return act;
}

void aim_activity_actor::start( player_activity &/*act*/, Character &/*who*/ )
{
    // Time spent on aiming is determined on the go by the player
    // Dummy progress task to indicate ongoing activity
    progress.dummy();
}

void aim_activity_actor::do_turn( player_activity &act, Character &who )
{
    if( !who.is_avatar() ) {
        debugmsg( "ACT_AIM not implemented for NPCs" );
        aborted = true;
        progress.pop();
        return;
    }
    avatar &you = get_avatar();

    item *weapon = get_weapon();
    if( !weapon || !avatar_action::can_fire_weapon( you, *weapon ) ) {
        aborted = true;
        progress.pop();
        return;
    }

    gun_mode gun = weapon->gun_current_mode();
    if( !gun->ammo_remaining() && !reload_loc && gun->has_flag( flag_RELOAD_AND_SHOOT ) ) {
        if( !load_RAS_weapon() ) {
            aborted = true;
            progress.pop();
            return;
        }
    }
    g->temp_exit_fullscreen();
    target_handler::trajectory trajectory;
    if( const auto shape_gen = ranged::get_shape_factory( *weapon ) ) {
        trajectory = target_handler::mode_shaped( you, *shape_gen, *this );
    } else {
        trajectory = target_handler::mode_fire( you, *this );
    }
    g->reenter_fullscreen();

    if( aborted ) {
        progress.pop();
    } else {
        if( !trajectory.empty() ) {
            fin_trajectory = trajectory;
            progress.pop();
        }

        // Allow interrupting activity only during 'aim and fire'.
        // Prevents '.' key for 'aim for 10 turns' from conflicting with '.' key for 'interrupt activity'
        // in case of high input lag (curses, sdl sometimes...), but allows to interrupt aiming
        // if a bug happens / stars align to cause an endless aiming loop.
        act.interruptable_with_kb = action != "AIM";
    }
}

void aim_activity_actor::finish( player_activity &act, Character &who )
{
    act.set_to_null();
    item *weapon = get_weapon();
    if( !weapon ) {
        restore_view();
        return;
    }
    if( aborted ) {
        if( reload_requested ) {
            // Reload the gun / select different arrows
            // May assign ACT_RELOAD
            avatar_action::reload_wielded( true );
        }
        restore_view();
        return;
    }

    // Fire!
    gun_mode gun = weapon->gun_current_mode();

    item *ammo_loc = reload_loc ? &*reload_loc : nullptr;

    int shots_fired = ranged::fire_gun( who, fin_trajectory.back(), gun.qty, *gun, ammo_loc );

    if( shots_fired > 0 ) {
        // TODO: bionic power cost of firing should be derived from a value of the relevant weapon.
        if( bp_cost_per_shot > 0_J ) {
            who.mod_power_level( -bp_cost_per_shot * shots_fired );
        }
        if( stamina_cost_per_shot > 0 ) {
            who.mod_stamina( -stamina_cost_per_shot * shots_fired );
        }
    }

    if( !get_option<bool>( "AIM_AFTER_FIRING" ) ||
        who.recoil <= ranged::calculate_aim_cap( who, fin_trajectory.back() ) ) {
        restore_view();
        return;
    }

    // re-enter aiming UI with same parameters
    std::unique_ptr<aim_activity_actor> aim_actor = std::make_unique<aim_activity_actor>();
    aim_actor->abort_if_no_targets = true;
    aim_actor->fake_weapon = std::move( this->fake_weapon );
    aim_actor->bp_cost_per_shot = this->bp_cost_per_shot;
    aim_actor->initial_view_offset = this->initial_view_offset;

    // if invalid target or it's dead - reset it so a new one is acquired
    shared_ptr_fast<Creature> last_target = who.last_target.lock();
    if( last_target && last_target->is_dead_state() ) {
        who.last_target.reset();
    }
    who.assign_activity( std::make_unique<player_activity>( std::move( aim_actor ) ), false );
}

void aim_activity_actor::canceled( player_activity &/*act*/, Character &/*who*/ )
{
    restore_view();
}

void aim_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();

    jsout.member( "progress", progress );
    jsout.member( "fake_weapon", fake_weapon ? *fake_weapon : null_item_reference() );
    jsout.member( "bp_cost_per_shot", bp_cost_per_shot );
    jsout.member( "stamina_cost_per_shot", stamina_cost_per_shot );
    jsout.member( "action", action );
    jsout.member( "aif_duration", aif_duration );
    jsout.member( "aiming_at_critter", aiming_at_critter );
    jsout.member( "snap_to_target", snap_to_target );
    jsout.member( "shifting_view", shifting_view );
    jsout.member( "initial_view_offset", initial_view_offset );
    jsout.member( "loaded_RAS_weapon", loaded_RAS_weapon );
    jsout.member( "reload_loc", reload_loc );
    jsout.member( "aborted", aborted );
    jsout.member( "reload_requested", reload_requested );
    jsout.member( "abort_if_no_targets", abort_if_no_targets );

    jsout.end_object();
}

std::unique_ptr<activity_actor> aim_activity_actor::deserialize( JsonIn &jsin )
{
    std::unique_ptr<aim_activity_actor> actor( new aim_activity_actor() );

    JsonObject data = jsin.get_object();

    data.read( "progress", actor->progress );
    data.read( "fake_weapon", actor->fake_weapon );
    data.read( "bp_cost_per_shot", actor->bp_cost_per_shot );
    data.read( "stamina_cost_per_shot", actor->stamina_cost_per_shot );
    data.read( "action", actor->action );
    data.read( "aif_duration", actor->aif_duration );
    data.read( "aiming_at_critter", actor->aiming_at_critter );
    data.read( "snap_to_target", actor->snap_to_target );
    data.read( "shifting_view", actor->shifting_view );
    data.read( "initial_view_offset", actor->initial_view_offset );
    data.read( "loaded_RAS_weapon", actor->loaded_RAS_weapon );
    data.read( "reload_loc", actor->reload_loc );
    data.read( "aborted", actor->aborted );
    data.read( "reload_requested", actor->reload_requested );
    data.read( "abort_if_no_targets", actor->abort_if_no_targets );

    return actor;
}

item *aim_activity_actor::get_weapon()
{
    if( weapon ) {
        return &*weapon;
    }
    if( fake_weapon ) {
        // TODO: check if the player lost relevant bionic/mutation
        return &*fake_weapon;
    } else {
        // Check for lost gun (e.g. yanked by zombie technician)
        // TODO: check that this is the same gun that was used to start aiming
        item *weapon = &get_player_character().primary_weapon();
        return weapon->is_null() ? nullptr : weapon;
    }
}

void aim_activity_actor::restore_view()
{
    avatar &player_character = get_avatar();
    bool changed_z = player_character.view_offset.z() != initial_view_offset.z();
    player_character.view_offset = initial_view_offset;
    if( changed_z ) {
        get_map().invalidate_map_cache( player_character.view_offset.z() );
        g->invalidate_main_ui_adaptor();
    }
}

bool aim_activity_actor::load_RAS_weapon()
{
    // TODO: use activity for fetching ammo and loading weapon
    player &you = get_avatar();
    item *weapon = get_weapon();
    gun_mode gun = weapon->gun_current_mode();

    // Will burn (0.2% max base stamina * the strength required to fire)
    stamina_cost_per_shot = gun->get_min_str() * static_cast<int>
                            ( 0.002f * get_option<int>( "PLAYER_MAX_STAMINA" ) );
    if( you.get_stamina() < stamina_cost_per_shot ) {
        you.add_msg_if_player( m_bad, _( "You're too tired to draw your %s." ), weapon->tname() );
        return false;
    }

    const auto ammo_location_is_valid = [&]() -> bool {
        if( !you.ammo_location )
        {
            return false;
        }
        if( !gun->can_reload_with( you.ammo_location->typeId() ) )
        {
            return false;
        }
        if( square_dist( you.abs_pos(), you.ammo_location->abs_pos() ) > 1 )
        {
            return false;
        }
        return true;
    };
    item_reload_option opt = ammo_location_is_valid() ? item_reload_option( &you, weapon,
                             weapon, *you.ammo_location ) : character_funcs::select_ammo( you, *gun );
    if( !opt ) {
        // Menu canceled
        return false;
    }

    reload_loc = opt.ammo;
    loaded_RAS_weapon = true;
    return true;
}

void autodrive_activity_actor::start( player_activity &/* act */, Character &who )
{
    const bool in_vehicle = who.in_vehicle && who.controlling_vehicle;
    const optional_vpart_position vp = get_map().veh_at( who.bub_pos() );
    if( !( vp && in_vehicle ) ) {
        who.cancel_activity();
        return;
    }

    player_vehicle = &vp->vehicle();
    if( player_vehicle->is_flying_in_air() ) {
        int min_speed = player_vehicle->get_takeoff_speed( "t/t" );
        if( player_vehicle->velocity * 0.8 < min_speed * vehicles::cmps_per_tile ) {
            if( !g->u.query_yn( "Warning: Current Speed is below recommened values, proceed?" ) ) {
                who.cancel_activity();
                return;
            }
        }
        if( player_vehicle->min_autodrive_speed * 0.8 < min_speed ) {
            if( !g->u.query_yn( "Warning: Min Autodrive Speed is below recommened values, proceed?" ) ) {
                who.cancel_activity();
                return;
            }
        }
        if( player_vehicle->max_autodrive_speed * 0.5 < min_speed ) {
            if( !g->u.query_yn( "Warning: Max Autodrive Speed is below recommened values, proceed?" ) ) {
                who.cancel_activity();
                return;
            }
        }
    }
    player_vehicle->is_autodriving = true;
    progress.dummy();
}

void autodrive_activity_actor::do_turn( player_activity &/* act */, Character &who )
{
    if( who.in_vehicle && who.controlling_vehicle && player_vehicle ) {
        if( who.moves <= 0 ) {
            // out of moves? the driver's not doing anything this turn
            // (but the vehicle will continue moving)
            return;
        }
        switch( player_vehicle->do_autodrive( who ) ) {
            case autodrive_result::ok:
                if( who.moves > 0 ) {
                    // if do_autodrive() didn't eat up all our moves, end the turn
                    // equivalent to player pressing the "pause" button
                    who.moves = 0;
                }
                sounds::reset_markers();
                break;
            case autodrive_result::abort:
                who.cancel_activity();
                break;
            case autodrive_result::finished:
                progress.pop();
                break;
        }
    } else {
        who.cancel_activity();
    }
}

void autodrive_activity_actor::canceled( player_activity &act, Character &who )
{
    who.add_msg_if_player( m_info, _( "Auto-drive canceled." ) );
    who.omt_path.clear();
    if( player_vehicle ) {
        player_vehicle->stop_autodriving( false );
    }
    act.set_to_null();
}

void autodrive_activity_actor::finish( player_activity &act, Character &who )
{
    who.add_msg_if_player( m_info, _( "You have reached your destination." ) );
    player_vehicle->stop_autodriving( false );
    act.set_to_null();
}

void autodrive_activity_actor::serialize( JsonOut &jsout ) const
{
    // Activity is not being saved but still provide some valid json if called.
    jsout.write_null();
}

std::unique_ptr<activity_actor> autodrive_activity_actor::deserialize( JsonIn & )
{
    return std::make_unique<autodrive_activity_actor>();
}

void dig_activity_actor::start( player_activity &act, Character &who )
{
    auto &here = who.get_mapbuffer();
    auto handle = abs_tile_handle::fetch( here, location );
    if( !handle ) {
        act.set_to_null();
        return;
    }
    ter_id ter_here = handle->ter();
    const bool grave = ter_here == t_grave;
    const std::string name = grave
                             ? "grave"
                             : ter_here->name();
    progress.emplace( name, moves_total );
}

void dig_activity_actor::do_turn( player_activity &/*act*/, Character &who )
{
    if( progress.front().complete() ) {
        progress.pop();
        return;
    }
    sfx::play_activity_sound( "tool", "shovel", sfx::get_heard_volume( abs_to_bub( location ), 60 ) );
    if( action_time_scale::once_every_this_tick( 1_minutes ) ) {
        //~ Sound of a shovel digging a pit at work!
        sound_event se;
        se.origin = location;
        se.volume = 60;
        se.category = sounds::sound_t::activity;
        se.description = _( "hsh!" );
        se.id =  "tool";
        se.variant = "shovel";
        se.from_player = who.is_player();
        se.from_npc = !se.from_player;
        se.faction = who.get_faction()->id;
        se.monfaction = who.get_faction()->mon_faction;
        sounds::sound( se );
    }
}

void dig_activity_actor::finish( player_activity &act, Character &who )
{
    auto &here = who.get_mapbuffer();
    auto handle = abs_tile_handle::fetch( here, location );
    if( !handle ) {
        act.set_to_null();
        return;
    }
    const bool grave = handle->ter() == t_grave;

    if( grave ) {
        if( one_in( 10 ) ) {
            static const std::array<mtype_id, 5> monids = { {
                    mon_zombie, mon_zombie_fat,
                    mon_zombie_rot, mon_skeleton,
                    mon_zombie_crawler
                }
            };

            here.place_critter_at( random_entry( monids ), byproducts_location );
            here.set_furn( location, f_coffin_o );
            who.add_msg_if_player( m_warning, _( "Something crawls out of the coffin!" ) );
        } else {
            here.spawn_item( location, itype_bone_human, rng( 5, 15 ) );
            here.set_furn( location, f_coffin_c );
        }
        std::vector<item *> dropped = here.place_items( item_group_id( "allclothes" ), 50, location,
                                      location, false,
                                      calendar::turn );
        here.place_items( item_group_id( "grave" ), 25, location, location, false, calendar::turn );
        here.place_items( item_group_id( "jewelry_front" ), 20, location, location, false,
                               calendar::turn );
        for( item * const &it : dropped ) {
            if( it->is_armor() ) {
                it->set_damage( rng( 1, it->max_damage() - 1 ) );
            }
        }
        g->events().send<event_type::exhumes_grave>( who.getID() );
    }

    here.set_ter( location, ter_id( result_terrain ) );

    here.spawn_items( byproducts_location,
                      item_group::items_from( item_group_id( byproducts_item_group ),
                              calendar::turn ) );

    const int act_exertion = act.moves_total;

    who.mod_stored_kcal( std::min( -1, -act_exertion / to_moves<int>( 80_seconds ) ) );
    who.mod_thirst( std::max( 1, act_exertion / to_moves<int>( 12_minutes ) ) );
    who.mod_fatigue( std::max( 1, act_exertion / to_moves<int>( 6_minutes ) ) );
    if( grave ) {
        who.add_msg_if_player( m_good, _( "You finish exhuming a grave." ) );
    } else {
        who.add_msg_if_player( m_good, _( "You finish digging the %s." ),
                               handle->tername() );
    }

    act.set_to_null();
}

void dig_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();

    jsout.member( "progress", progress );
    jsout.member( "moves", moves_total );
    jsout.member( "location", location );
    jsout.member( "result_terrain", result_terrain );
    jsout.member( "byproducts_location", byproducts_location );
    jsout.member( "byproducts_item_group", byproducts_item_group );

    jsout.end_object();
}

std::unique_ptr<activity_actor> dig_activity_actor::deserialize( JsonIn &jsin )
{
    std::unique_ptr<dig_activity_actor> actor( new dig_activity_actor( 0, tripoint_abs_ms::zero(),
            {}, tripoint_abs_ms::zero(), {} ) );

    JsonObject data = jsin.get_object();

    data.read( "progress", actor->progress );
    data.read( "moves", actor->moves_total );
    data.read( "location", actor->location );
    data.read( "result_terrain", actor->result_terrain );
    data.read( "byproducts_location", actor->byproducts_location );
    data.read( "byproducts_item_group", actor->byproducts_item_group );

    return actor;
}

void dig_channel_activity_actor::start( player_activity &act, Character &who )
{
    auto &here = who.get_mapbuffer();
    auto handle = abs_tile_handle::fetch( here, location );
    if( !handle ) {
        act.set_to_null();
        return;
    }
    progress.emplace( handle->tername(), moves_total );
}

void dig_channel_activity_actor::do_turn( player_activity &/*act*/, Character &who )
{
    if( progress.front().complete() ) {
        progress.pop();
        return;
    }
    sfx::play_activity_sound( "tool", "shovel", sfx::get_heard_volume( abs_to_bub( location ), 70 ) );
    if( action_time_scale::once_every_this_tick( 1_minutes ) ) {
        //~ Sound of a shovel digging a pit at work!
        sound_event se;
        se.origin = location;
        se.volume = 70;
        se.category = sounds::sound_t::activity;
        se.description = _( "hsh!" );
        se.id =  "tool";
        se.variant =  "shovel";
        se.from_player = who.is_player();
        se.from_npc = !se.from_player;
        se.faction = who.get_faction()->id;
        se.monfaction = who.get_faction()->mon_faction;
        sounds::sound( se );
    }
}

void dig_channel_activity_actor::finish( player_activity &act, Character &who )
{
    auto &here = who.get_mapbuffer();
    auto handle = abs_tile_handle::fetch( here, location );
    if( !handle ) {
        act.set_to_null();
        return;
    }
    here.set_ter( location, ter_id( result_terrain ) );

    here.spawn_items( byproducts_location,
                      item_group::items_from( item_group_id( byproducts_item_group ),
                              calendar::turn ) );

    const int act_exertion = act.moves_total;

    who.mod_stored_kcal( std::min( -1, -act_exertion / to_moves<int>( 80_seconds ) ) );
    who.mod_thirst( std::max( 1, act_exertion / to_moves<int>( 12_minutes ) ) );
    who.mod_fatigue( std::max( 1, act_exertion / to_moves<int>( 6_minutes ) ) );
    who.add_msg_if_player( m_good, _( "You finish digging up %s." ),
                           here.ter( location )->obj().name() );

    act.set_to_null();
}

void dig_channel_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();

    jsout.member( "progress", progress );
    jsout.member( "moves", moves_total );
    jsout.member( "location", location );
    jsout.member( "result_terrain", result_terrain );
    jsout.member( "byproducts_location", byproducts_location );
    jsout.member( "byproducts_item_group", byproducts_item_group );

    jsout.end_object();
}

std::unique_ptr<activity_actor> dig_channel_activity_actor::deserialize( JsonIn &jsin )
{
    std::unique_ptr<dig_channel_activity_actor> actor( new dig_channel_activity_actor( 0,
            tripoint_abs_ms::zero(),
            {}, tripoint_abs_ms::zero(), {} ) );

    JsonObject data = jsin.get_object();

    data.read( "progress", actor->progress );
    data.read( "moves", actor->moves_total );
    data.read( "location", actor->location );
    data.read( "result_terrain", actor->result_terrain );
    data.read( "byproducts_location", actor->byproducts_location );
    data.read( "byproducts_item_group", actor->byproducts_item_group );

    return actor;
}

bool disassemble_activity_actor::try_start_single( player_activity &/* act */, Character &who )
{
    if( targets.empty() ) {
        return false;
    }
    const iuse_location &target = targets.front();
    if( !target.loc ) {
        debugmsg( "Lost target of ACT_DISASSEMBLE" );
        targets.clear();
        return false;
    }
    const item &itm = *target.loc;

    // Have to check here again in case we ran out of tools
    const ret_val<bool> can_do = crafting::can_disassemble( who, itm, who.crafting_inventory() );
    if( !can_do.success() ) {
        who.add_msg_if_player( m_info, "%s", can_do.c_str() );
        return false;
    }
    return true;
}

inline void disassemble_activity_actor::process_target( player_activity &/*act*/,
        iuse_location &target )
{
    const item &itm = *target.loc;
    const recipe &dis = recipe_dictionary::get_uncraft( itm.typeId() );
    int moves_needed = dis.time * target.count;
    progress.emplace( itm.tname( target.count ), moves_needed );
}

inline void disassemble_activity_actor::calc_all_moves( player_activity &act, Character &who )
{
    const auto &target = targets.front().loc;
    auto reqs = activity_reqs_adapter( recipe_dictionary::get_uncraft( target->typeId() ),
                                       std::make_pair( target->weight(), target->volume() ) );
    act.speed.calc_all_moves( who, reqs );
}

void disassemble_activity_actor::start( player_activity &act, Character &who )
{
    if( !who.is_avatar() ) {
        debugmsg( "ACT_DISASSEMBLE is not implemented for NPCs" );
        act.set_to_null();
    } else if( !try_start_single( act, who ) ) {
        act.set_to_null();
    }
    for( auto &target : targets ) {
        process_target( act, target );
    }
}

void disassemble_activity_actor::do_turn( player_activity &act, Character &who )
{
    if( progress.front().complete() ) {
        const iuse_location &target = targets.front();
        if( !target.loc ) {
            debugmsg( "Lost target of ACT_DISASSEMBLY" );
        } else {
            crafting::complete_disassemble( who, target, abs_to_bub( pos ) );
        }
        targets.erase( targets.begin() );
        progress.pop();
        if( !progress.empty() ) {
            if( try_start_single( act, who ) ) {
                calc_all_moves( act, who );
            } else {
                act.set_to_null();
            }
        }
    }
}

void disassemble_activity_actor::finish( player_activity &act, Character &who )
{
    if( try_start_single( act, who ) ) {
        debugmsg( "disassemble_activity_actor call finish function while able to start new disassembly" );
    }
    // Make a copy to avoid use-after-free
    bool recurse = this->recursive;

    act.set_to_null();

    if( recurse ) {
        crafting::disassemble_all( *who.as_avatar(), recurse );
    }
}

void disassemble_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();

    jsout.member( "progress", progress );
    jsout.member( "targets", targets );
    jsout.member( "pos", pos );
    jsout.member( "recursive", recursive );

    jsout.end_object();
}

std::unique_ptr<activity_actor> disassemble_activity_actor::deserialize( JsonIn &jsin )
{
    std::unique_ptr<disassemble_activity_actor> actor( new disassemble_activity_actor() );

    JsonObject data = jsin.get_object();

    data.read( "progress", actor->progress );
    data.read( "targets", actor->targets );
    data.read( "pos", actor->pos );
    data.read( "recursive", actor->recursive );

    return actor;
}

drop_activity_actor::drop_activity_actor( Character &ch, const drop_locations &items,
        bool force_ground, const tripoint_rel_ms &relpos )
    : force_ground( force_ground ), relpos( relpos )
{
    this->items = pickup::reorder_for_dropping( ch, items );
}

void drop_activity_actor::start( player_activity &/* act */, Character & )
{
    // Dummy progress task to indicate ongoing activity
    progress.dummy();
}

void drop_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();

    jsout.member( "progress", progress );
    jsout.member( "items", items );
    jsout.member( "force_ground", force_ground );
    jsout.member( "relpos", relpos );

    jsout.end_object();
}

std::unique_ptr<activity_actor> drop_activity_actor::deserialize( JsonIn &jsin )
{
    std::unique_ptr<drop_activity_actor> actor( new drop_activity_actor() );

    JsonObject data = jsin.get_object();

    data.read( "progress", actor->progress );
    data.read( "items", actor->items );
    data.read( "force_ground", actor->force_ground );
    data.read( "relpos", actor->relpos );

    return actor;
}

enum hack_result {
    HACK_UNABLE,
    HACK_FAIL,
    HACK_NOTHING,
    HACK_SUCCESS
};

enum hack_type {
    HACK_SAFE,
    HACK_DOOR,
    HACK_GAS,
    HACK_NULL
};

static hack_type get_hack_type( tripoint_bub_ms examp )
{
    hack_type type = HACK_NULL;
    const map &here = get_map();
    const furn_t &xfurn_t = *here.furn( examp );
    const ter_t &xter_t = *here.ter( examp );
    if( xter_t.examine == &iexamine::pay_gas || xfurn_t.examine == &iexamine::pay_gas ) {
        type = HACK_GAS;
    } else if( xter_t.examine == &iexamine::cardreader || xfurn_t.examine == &iexamine::cardreader ) {
        type = HACK_DOOR;
    } else if( xter_t.examine == &iexamine::gunsafe_el || xfurn_t.examine == &iexamine::gunsafe_el ) {
        type = HACK_SAFE;
    }
    return type;
}

void hacking_activity_actor::start( player_activity &act, Character & )
{
    hack_type type = get_hack_type( abs_to_bub( act.placement ) );
    std::string name;

    switch( type ) {
        case hack_type::HACK_SAFE:
            name = "safe";
            break;
        case hack_type::HACK_DOOR:
            name = "door panel";
            break;
        case hack_type::HACK_GAS:
            name = "gas pump";
            break;
        default:
            name = "";
            break;
    }

    progress.emplace( name, to_moves<int>( 5_minutes ) );
}

void hacking_activity_actor::do_turn( player_activity &/*act*/, Character & )
{
    if( progress.front().complete() ) {
        progress.pop();
        return;
    }
}

static int hack_level( const Character &who )
{
    ///\EFFECT_COMPUTER increases success chance of hacking card readers
    // odds go up with int>8, down with int<8
    // 4 int stat is worth 1 computer skill here
    ///\EFFECT_INT increases success chance of hacking card readers
    return who.get_skill_level( skill_computer ) + ( who.int_cur - 8 ) / 4;
}

static hack_result hack_attempt( Character &who, const bool using_bionic )
{
    who.practice( skill_computer, 20 );
    // only skilled supergenius never cause short circuits, but the odds are low for people
    // with moderate skills
    const int hack_stddev = 5;
    int success = std::ceil( normal_roll( hack_level( who ), hack_stddev ) );
    if( success < 0 ) {
        who.add_msg_if_player( _( "You cause a short circuit!" ) );
        if( using_bionic ) {
            who.mod_power_level( -25_kJ );
        } else {
            who.use_charges( itype_electrohack, 25 );
        }

        if( success <= -5 ) {
            if( !using_bionic ) {
                who.add_msg_if_player( m_bad, _( "Your electrohack is ruined!" ) );
                who.use_amount( itype_electrohack, 1 );
            } else {
                who.add_msg_if_player( m_bad, _( "Your power is drained!" ) );
                who.mod_power_level( units::from_kilojoule( -rng( 25,
                                     units::to_kilojoule( who.get_power_level() ) ) ) );
            }
        }
        return HACK_FAIL;
    } else if( success < 6 ) {
        return HACK_NOTHING;
    } else {
        return HACK_SUCCESS;
    }
}

hacking_activity_actor::hacking_activity_actor( use_bionic )
    : using_bionic( true )
{
}

void hacking_activity_actor::finish( player_activity &act, Character &who )
{
    tripoint_bub_ms examp = abs_to_bub( act.placement );
    hack_type type = get_hack_type( examp );
    map &here = get_map();
    sound_event se;
    switch( hack_attempt( who, using_bionic ) ) {
        case HACK_UNABLE:
            who.add_msg_if_player( _( "You cannot hack this." ) );
            break;
        case HACK_FAIL:
            // currently all things that can be hacked have equivalent alarm failure states.
            // this may not always be the case with new hackable things.
            g->events().send<event_type::triggers_alarm>( who.getID() );
            se.origin = who.abs_pos();
            se.volume = 120;
            se.category = sounds::sound_t::music;
            se.description = _( "an alarm sound!" );
            se.id = "environment";
            se.variant = "alarm";
            sounds::sound( se );
            if( examp.z() > 0 && !g->timed_events.queued( TIMED_EVENT_WANTED ) ) {
                g->timed_events.add( TIMED_EVENT_WANTED, calendar::turn + 30_minutes, 0,
                                     who.abs_sm_pos() );
            }
            break;
        case HACK_NOTHING:
            who.add_msg_if_player( _( "You fail the hack, but no alarms are triggered." ) );
            break;
        case HACK_SUCCESS:
            if( type == HACK_GAS ) {
                int tankGasUnits;
                const std::optional<tripoint_bub_ms> pTank_ = iexamine::getNearFilledGasTank( examp, tankGasUnits );
                if( !pTank_ ) {
                    break;
                }
                const tripoint_bub_ms pTank = *pTank_;
                const std::optional<tripoint_bub_ms> pGasPump = iexamine::getGasPumpByNumber( examp,
                        uistate.ags_pay_gas_selected_pump );
                if( pGasPump && iexamine::toPumpFuel( pTank, *pGasPump, tankGasUnits ) ) {
                    who.add_msg_if_player( _( "You hack the terminal and route all available fuel to your pump!" ) );
                    se.origin = bub_to_abs( examp );
                    se.volume = 40;
                    se.category = sounds::sound_t::activity;
                    se.description = _( "Glug Glug Glug Glug Glug Glug Glug Glug Glug" );
                    se.id = "tool";
                    se.variant =  "gaspump";
                    se.from_player = who.is_player();
                    se.from_npc = !se.from_player;
                    se.faction = who.get_faction()->id;
                    se.monfaction = who.get_faction()->mon_faction;
                    sounds::sound( se );
                } else {
                    who.add_msg_if_player( _( "Nothing happens." ) );
                }
            } else if( type == HACK_SAFE ) {
                who.add_msg_if_player( m_good, _( "The door on the safe swings open." ) );
                here.furn_set( examp, furn_str_id( "f_gunsafe_o" ) );
            } else if( type == HACK_DOOR ) {
                who.add_msg_if_player( _( "You activate the panel!" ) );
                who.add_msg_if_player( m_good, _( "The nearby doors unlock." ) );
                here.ter_set( examp, t_card_reader_broken );
                for( const tripoint_bub_ms &tmp : here.points_in_radius( ( examp ), 3 ) ) {
                    if( here.ter( tmp ) == t_door_metal_locked ) {
                        here.ter_set( tmp, t_door_metal_c );
                    }
                }
            }
            break;
    }
    act.set_to_null();
}

void hacking_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();

    jsout.member( "progress", progress );
    jsout.member( "using_bionic", using_bionic );

    jsout.end_object();
}

std::unique_ptr<activity_actor> hacking_activity_actor::deserialize( JsonIn &jsin )
{
    std::unique_ptr<hacking_activity_actor> actor( new hacking_activity_actor() );
    if( jsin.test_null() ) {
        // Old saves might contain a null instead of an object.
        // Since we do not know whether a bionic or an item was chosen we assume
        // it was an item.
        actor->using_bionic = false;
    } else {
        JsonObject jsobj = jsin.get_object();
        jsobj.read( "using_bionic", actor->using_bionic );
        jsobj.read( "progress", actor->progress );
    }
    return actor;
}

void move_items_activity_actor::do_turn( player_activity &act, Character &who )
{
    const auto dest = relative_destination + who.abs_pos();

    while( who.moves > 0 && !target_items.empty() ) {
        safe_reference<item> target = std::move( target_items.back() );
        const int quantity = quantities.back();
        target_items.pop_back();
        quantities.pop_back();

        if( !target ) {
            //TODO!: might not be appropriate to debugmsg just because something was destroyed/unloaded
            debugmsg( "Lost target item of ACT_MOVE_ITEMS" );
            continue;
        }

        // Check that we can pick it up.
        if( target->made_of( LIQUID ) ) {
            continue;
        }

        // This is for hauling across zlevels, remove when going up and down stairs
        // is no longer teleportation
        // Also ignores items owned by other NPCs, unless they'd already attack on sight
        if( target->is_owned_by( who, true ) || target->get_owner()->likes_u < -10 ) {
            target->set_owner( who );
        } else {
            continue;
        }

        const auto src = target->abs_pos();
        detached_ptr<item> newit = quantity == 0 ? target->detach() : target->split( quantity );

        const int distance = src.z() == dest.z() ? std::max( rl_dist( src, dest ), 1 ) : 1;
        who.mod_moves( -pickup::cost_to_move_item( who, *newit ) * distance );

        std::vector<detached_ptr<item>> vec;
        vec.push_back( std::move( newit ) );
        if( to_vehicle ) {
            put_into_vehicle_or_drop( who, item_drop_reason::deliberate, vec, abs_to_bub( dest ) );
        } else {
            drop_on_map( who, item_drop_reason::deliberate, vec, abs_to_bub( dest ) );
        }
    }

    if( target_items.empty() ) {
        // Nuke the current activity, leaving the backlog alone.
        act.set_to_null();
        if( who.is_hauling() && !has_haulable_items( who.bub_pos() ) ) {
            who.stop_hauling();
        }
    }
}

void move_items_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();

    jsout.member( "progress", progress );
    jsout.member( "target_items", target_items );
    jsout.member( "quantities", quantities );
    jsout.member( "to_vehicle", to_vehicle );
    jsout.member( "relative_destination", relative_destination );

    jsout.end_object();
}

std::unique_ptr<activity_actor> move_items_activity_actor::deserialize( JsonIn &jsin )
{
    std::unique_ptr<move_items_activity_actor> actor( new move_items_activity_actor( {}, {}, false,
            tripoint_rel_ms::zero() ) );

    JsonObject data = jsin.get_object();

    data.read( "progress", actor->progress );
    data.read( "target_items", actor->target_items );
    data.read( "quantities", actor->quantities );
    data.read( "to_vehicle", actor->to_vehicle );
    data.read( "relative_destination", actor->relative_destination );

    return actor;
}

void pickup_activity_actor::do_turn( player_activity &act, Character &who )
{
    // If we don't have target items bail out
    if( target_items.empty() ) {
        who.cancel_activity();
        return;
    }

    // If the player moves while picking up (i.e.: in a moving vehicle) cancel
    // the activity, only populate starting_pos when grabbing from the ground
    if( starting_pos && *starting_pos != who.abs_pos() ) {
        who.cancel_activity();
        who.add_msg_if_player( _( "Moving canceled auto-pickup." ) );
        return;
    }

    // Auto_resume implies autopickup.
    const bool autopickup = who.activity->auto_resume;

    // False indicates that the player canceled pickup when met with some prompt
    const bool keep_going = pickup::do_pickup( target_items, autopickup );

    // Check thievey witness
    npc *witness = nullptr;
    if( !act.str_values.empty() && act.str_values[0] == has_thievery_witness ) {
        for( auto &guy : who.get_mapbuffer().all_npcs() ) {
            if( guy->get_attitude() == NPCATT_RECOVER_GOODS ) {
                witness = guy.get();
                break;
            }
        }
    }

    // If there are items left we ran out of moves, so continue the activity
    // Otherwise, we are done.
    if( !keep_going || target_items.empty() || witness ) {
        who.cancel_activity();

        if( who.get_value( "THIEF_MODE_KEEP" ) != "YES" ) {
            who.set_value( "THIEF_MODE", "THIEF_ASK" );
        }

        if( !keep_going ) {
            // The user canceled the activity, so we're done
            // AIM might have more pickup activities pending, also cancel them.
            // TODO: Move this to advanced inventory instead of hacking it in here
            cancel_aim_processing();
        }

        if( witness ) {
            witness->talk_to_u();
            // Then remove "has_thievery_witness" from the activity
            act.str_values.clear();
        }
    }
}

void pickup_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();

    jsout.member( "progress", progress );
    jsout.member( "target_items", target_items );
    jsout.member( "starting_pos", starting_pos );

    jsout.end_object();
}

std::unique_ptr<activity_actor> pickup_activity_actor::deserialize( JsonIn &jsin )
{
    std::unique_ptr<pickup_activity_actor> actor( new pickup_activity_actor( {}, std::nullopt ) );

    JsonObject data = jsin.get_object();

    data.read( "progress", actor->progress );
    data.read( "target_items", actor->target_items );
    data.read( "starting_pos", actor->starting_pos );

    return actor;
}

void hacksaw_activity_actor::start( player_activity &act, Character &who )
{
    auto &here = who.get_mapbuffer();
    auto handle = abs_tile_handle::fetch( here, target );
    if( !handle ) {
        act.set_to_null();
        return;
    }

    if( handle->furn() != f_null ) {
        const furn_id furn_type = handle->furn();
        if( !furn_type->hacksaw->valid() ) {
            if( !testing ) {
                debugmsg( "%s hacksaw is invalid", furn_type.id().str() );
            }
            act.set_to_null();
            return;
        }
        progress.emplace( furn_type->name(), to_moves<int>( furn_type->hacksaw->duration() ) );
    } else if( !handle->ter()->is_null() ) {
        const ter_id ter_type = handle->ter();
        if( !ter_type->hacksaw->valid() ) {
            if( !testing ) {
                debugmsg( "%s hacksaw is invalid", ter_type.id().str() );
            }
            act.set_to_null();
            return;
        }
        progress.emplace( ter_type->name(), to_moves<int>( ter_type->hacksaw->duration() ) );
    } else {
        if( !testing ) {
            debugmsg( "hacksaw activity called on invalid terrain" );
        }
        act.set_to_null();
        return;
    }
}

void hacksaw_activity_actor::do_turn( player_activity &/* act */, Character &who )
{
    if( progress.front().complete() ) {
        progress.pop();
        return;
    }
    if( tool->ammo_sufficient() ) {
        tool->ammo_consume( tool->ammo_required() );
        sfx::play_activity_sound( "tool", "hacksaw", sfx::get_heard_volume( abs_to_bub( target ), 80 ) );
        if( action_time_scale::once_every_this_tick( 1_minutes ) ) {
            //~ Sound of a metal sawing tool at work!
            sound_event se;
            se.origin = target;
            se.volume = 80;
            se.category = sounds::sound_t::destructive_activity;
            se.description = _( "grnd grnd grnd" );
            se.id = "tool";
            se.variant = "hacksaw";
            se.from_player = who.is_player();
            se.from_npc = !se.from_player;
            se.faction = who.get_faction()->id;
            se.monfaction = who.get_faction()->mon_faction;
            sounds::sound( se );
        }
    } else {
        if( who.is_avatar() ) {
            who.add_msg_if_player( m_bad, _( "Your %1$s ran out of charges." ), tool->tname() );
        } else { // who.is_npc()
            if( get_avatar().sees( who.abs_pos() ) ) {
                add_msg( _( "%1$s %2$s ran out of charges." ), who.disp_name( false,
                         true ), tool->tname() );
            }
        }
        who.cancel_activity();
    }
}

void hacksaw_activity_actor::finish( player_activity &act, Character &who )
{
    auto &here = who.get_mapbuffer();
    auto handle = abs_tile_handle::fetch( here, target );
    if( !handle ) {
        act.set_to_null();
        return;
    }
    const activity_data_common *data;

    if( handle->furn() != f_null ) {
        const furn_id furn_type = handle->furn();
        if( !furn_type->hacksaw->valid() ) {
            if( !testing ) {
                debugmsg( "%s hacksaw is invalid", furn_type.id().str() );
            }
            act.set_to_null();
            return;
        }

        const furn_str_id new_furn = furn_type->hacksaw->result();
        if( !new_furn.is_valid() ) {
            if( !testing ) {
                debugmsg( "hacksaw furniture: %s invalid furniture", new_furn.str() );
            }
            act.set_to_null();
            return;
        }

        data = static_cast<const activity_data_common *>( &*furn_type->hacksaw );
        here.set_furn( target, new_furn );
    } else if( !here.ter( target )->obj().is_null() ) {
        const ter_id ter_type = *here.ter( target );
        if( !ter_type->hacksaw->valid() ) {
            if( !testing ) {
                debugmsg( "%s hacksaw is invalid", ter_type.id().str() );
            }
            act.set_to_null();
            return;
        }

        const ter_str_id new_ter = ter_type->hacksaw->result();
        if( !new_ter.is_valid() ) {
            if( !testing ) {
                debugmsg( "hacksaw terrain: %s invalid terrain", new_ter.str() );
            }
            act.set_to_null();
            return;
        }

        data = static_cast<const activity_data_common *>( &*ter_type->hacksaw );
        here.set_ter( target, new_ter );
    } else {
        if( !testing ) {
            debugmsg( "hacksaw activity finished on invalid terrain" );
        }
        act.set_to_null();
        return;
    }

    for( const activity_byproduct &byproduct : data->byproducts() ) {
        const int amount = byproduct.roll();
        if( byproduct.item->count_by_charges() ) {
            here.add_item_or_charges( target, item::spawn( byproduct.item, calendar::turn, amount ) );
        } else {
            for( int i = 0; i < amount; ++i ) {
                here.add_item_or_charges( target, item::spawn( byproduct.item, calendar::turn ) );
            }
        }
    }

    if( !data->message().empty() ) {
        who.add_msg_if_player( m_info, data->message().translated() );
    }

    act.set_to_null();
}

void hacksaw_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();

    jsout.member( "progress", progress );
    jsout.member( "target", target );
    jsout.member( "tool", tool );

    jsout.end_object();
}

std::unique_ptr<activity_actor> hacksaw_activity_actor::deserialize( JsonIn &jsin )
{
    std::unique_ptr<hacksaw_activity_actor> actor( new hacksaw_activity_actor(
                tripoint_abs_ms::zero(), safe_reference<item>() ) );
    JsonObject data = jsin.get_object();
    data.read( "progress", actor->progress );
    data.read( "target", actor->target );
    data.read( "tool", actor->tool );
    return actor;
}

void boltcutting_activity_actor::start( player_activity &act, Character &who )
{
    auto &here = who.get_mapbuffer();
    auto handle = abs_tile_handle::fetch( here, target );
    if( !handle ) {
        act.set_to_null();
        return;
    }
    if( handle->furn() != f_null ) {
        const furn_id furn_type = handle->furn();
        if( !furn_type->boltcut->valid() ) {
            if( !testing ) {
                debugmsg( "%s boltcut is invalid", furn_type.id().str() );
            }
            act.set_to_null();
            return;
        }
        progress.emplace( furn_type->name(), to_moves<int>( furn_type->boltcut->duration() ) );
    } else if( !handle->ter()->is_null() ) {
        const ter_id ter_type = handle->ter();
        if( !ter_type->boltcut->valid() ) {
            if( !testing ) {
                debugmsg( "%s boltcut is invalid", ter_type.id().str() );
            }
            act.set_to_null();
            return;
        }
        progress.emplace( ter_type->name(), to_moves<int>( ter_type->boltcut->duration() ) );
    } else {
        if( !testing ) {
            debugmsg( "boltcut activity called on invalid terrain" );
        }
        act.set_to_null();
        return;
    }
}

void boltcutting_activity_actor::do_turn( player_activity &/* act */, Character &who )
{
    if( progress.front().complete() ) {
        progress.pop();
        return;
    }
    if( tool->ammo_sufficient() ) {
        tool->ammo_consume( tool->ammo_required() );
    } else {
        if( who.is_avatar() ) {
            who.add_msg_if_player( m_bad, _( "Your %1$s ran out of charges." ), tool->tname() );
        } else { // who.is_npc()
            if( get_avatar().sees( who.abs_pos() ) ) {
                add_msg( _( "%1$s %2$s ran out of charges." ), who.disp_name( false,
                         true ), tool->tname() );
            }
        }
        who.cancel_activity();
    }
}

void boltcutting_activity_actor::finish( player_activity &act, Character &who )
{
    auto &here = who.get_mapbuffer();
    auto handle = abs_tile_handle::fetch( here, target );
    if( !handle ) {
        act.set_to_null();
        return;
    }
    const activity_data_common *data;
    
    if( handle->furn() != f_null ) {
        const furn_id furn_type = handle->furn();
        if( !furn_type->boltcut->valid() ) {
            if( !testing ) {
                debugmsg( "%s boltcut is invalid", furn_type.id().str() );
            }
            act.set_to_null();
            return;
        }

        const furn_str_id new_furn = furn_type->boltcut->result();
        if( !new_furn.is_valid() ) {
            if( !testing ) {
                debugmsg( "boltcut furniture: %s invalid furniture", new_furn.str() );
            }
            act.set_to_null();
            return;
        }

        data = static_cast<const activity_data_common *>( &*furn_type->boltcut );
        here.set_furn( target, new_furn );
    } else if( !handle->ter()->is_null() ) {
        const ter_id ter_type = handle->ter();
        if( !ter_type->boltcut->valid() ) {
            if( !testing ) {
                debugmsg( "%s boltcut is invalid", ter_type.id().str() );
            }
            act.set_to_null();
            return;
        }

        const ter_str_id new_ter = ter_type->boltcut->result();
        if( !new_ter.is_valid() ) {
            if( !testing ) {
                debugmsg( "boltcut terrain: %s invalid terrain", new_ter.str() );
            }
            act.set_to_null();
            return;
        }

        data = static_cast<const activity_data_common *>( &*ter_type->boltcut );
        here.set_ter( target, new_ter );
    } else {
        if( !testing ) {
            debugmsg( "boltcut activity finished on invalid terrain" );
        }
        act.set_to_null();
        return;
    }
    sound_event se;
    se.origin = target;
    se.volume = 60;
    se.category = sounds::sound_t::combat;
    se.id = "tool";
    se.variant = "boltcutters";
    se.from_player = who.is_player();
    se.from_npc = !se.from_player;
    se.faction = who.get_faction()->id;
    se.monfaction = who.get_faction()->mon_faction;
    if( data->sound().empty() ) {
        se.description = _( "Snick, snick, gachunk!" );
        sounds::sound( se );
    } else {
        se.description = data->sound().translated();
        sounds::sound( se );
    }


    for( const activity_byproduct &byproduct : data->byproducts() ) {
        const int amount = byproduct.roll();
        if( byproduct.item->count_by_charges() ) {
            here.add_item_or_charges( target, item::spawn( byproduct.item, calendar::turn, amount ) );
        } else {
            for( int i = 0; i < amount; ++i ) {
                here.add_item_or_charges( target, item::spawn( byproduct.item, calendar::turn ) );
            }
        }
    }

    if( !data->message().empty() ) {
        who.add_msg_if_player( m_info, data->message().translated() );
    }

    act.set_to_null();
}

void boltcutting_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();

    jsout.member( "progress", progress );
    jsout.member( "target", target );
    jsout.member( "tool", tool );

    jsout.end_object();
}

std::unique_ptr<activity_actor> boltcutting_activity_actor::deserialize( JsonIn &jsin )
{
    std::unique_ptr<boltcutting_activity_actor> actor( new boltcutting_activity_actor(
                tripoint_abs_ms::zero(), safe_reference<item>() ) );

    JsonObject data = jsin.get_object();
    data.read( "progress", actor->progress );
    data.read( "target", actor->target );
    data.read( "tool", actor->tool );
    return actor;
}

std::unique_ptr<lockpick_activity_actor> lockpick_activity_actor::use_item(
    int moves_total,
    item &lockpick,
    const tripoint_abs_ms &target
)
{
    return std::unique_ptr<lockpick_activity_actor> ( new lockpick_activity_actor(
                moves_total,
                safe_reference<item>( lockpick ),
                detached_ptr<item>(),
                target
            ) );
}

std::unique_ptr<lockpick_activity_actor> lockpick_activity_actor::use_bionic(
    detached_ptr<item> &&fake_lockpick,
    const tripoint_abs_ms &target
)
{
    return std::unique_ptr<lockpick_activity_actor>( new lockpick_activity_actor(
                to_moves<int>( 5_seconds ),
                safe_reference<item>(),
                std::move( fake_lockpick ),
                target
            ) );
}

void lockpick_activity_actor::start( player_activity &/*act*/, Character &who )
{
    const auto target = abs_to_bub( this->target );
    const ter_id ter_type = get_map().ter( target );
    const furn_id furn_type = get_map().furn( target );
    const optional_vpart_position veh = get_map().veh_at( target );
    const auto door_lock = veh.part_with_feature( "DOOR_LOCKING", true );

    if( furn_type != f_null && !furn_type->lockpick_result.is_null() ) {
        progress.emplace( furn_type->name(), moves_total );
    } else if( veh && door_lock ) {
        progress.emplace( veh->vehicle().name, moves_total );
    } else {
        if( ter_type->lockpick_result.is_null() ) {
            debugmsg( "%s lockpick_result is null", ter_type.id().str() );
            return;
        }
        progress.emplace( ter_type->name(), moves_total );
    }
}

void lockpick_activity_actor::do_turn( player_activity &/* act */, Character & )
{
    if( progress.front().complete() ) {
        progress.pop();
        return;
    }
}

void lockpick_activity_actor::finish( player_activity &act, Character &who )
{
    act.set_to_null();

    item *it = nullptr;
    if( lockpick ) {
        it = &*lockpick;
    } else if( fake_lockpick ) {
        it = &*fake_lockpick;
    }

    if( !it ) {
        debugmsg( "Lost ACT_LOCKPICK item" );
        return;
    }

    const auto target = abs_to_bub( this->target );
    const ter_id ter_type = get_map().ter( target );
    const furn_id furn_type = get_map().furn( target );
    const optional_vpart_position veh = get_map().veh_at( target );
    const auto door_lock = veh.part_with_feature( "DOOR_LOCKING", true );

    ter_id new_ter_type = t_null;
    furn_id new_furn_type = f_null;
    std::string open_message = _( "The lock opens…" );

    if( furn_type != f_null ) {
        if( furn_type->lockpick_result.is_null() ) {
            debugmsg( "%s lockpick_result is null", furn_type.id().str() );
            return;
        }

        new_furn_type = furn_type->lockpick_result;
        if( !furn_type->lockpick_message.empty() ) {
            open_message = furn_type->lockpick_message.translated();
        }
    } else if( veh ) {
        if( !door_lock ) {
            debugmsg( "%s has no pickable part", furn_type.id().str() );
            return;
        }
    } else {
        if( ter_type->lockpick_result.is_null() ) {
            debugmsg( "%s lockpick_result is null", ter_type.id().str() );
            return;
        }

        new_ter_type = ter_type->lockpick_result;
        if( !ter_type->lockpick_message.empty() ) {
            open_message = ter_type->lockpick_message.translated();
        }
    }

    bool perfect = it->has_flag( flag_PERFECT_LOCKPICK );
    bool durable = it->has_flag( flag_DURABLE_LOCKPICK );
    bool destroy = false;

    /** @EFFECT_DEX improves chances of successfully picking door lock, reduces chances of bad outcomes */
    /** @EFFECT_MECHANICS improves chances of successfully picking door lock, reduces chances of bad outcomes */
    int pick_roll = 5 *
                    ( std::pow( 1.3, who.get_skill_level( skill_mechanics ) ) +
                      it->get_quality( qual_LOCKPICK ) - it->damage() / 2000.0 ) +
                    who.dex_cur / 4.0;
    int lock_roll = rng( 1, 120 );
    int xp_gain = 0;
    if( perfect || ( pick_roll >= lock_roll ) ) {
        xp_gain += lock_roll;

        if( furn_type != f_null ) {
            get_map().furn_set( target, new_furn_type );
        } else if( door_lock ) {
            door_lock->part().enabled = false;
        } else {
            get_map().ter_set( target, new_ter_type );
        }

        who.add_msg_if_player( m_good, open_message );
    } else if( lock_roll > ( 1.5 * pick_roll ) && !durable ) {
        // damage lockpick on a low result, unless it's durable
        if( it->inc_damage() ) {
            who.add_msg_if_player( m_bad,
                                   _( "The lock stumps your efforts to pick it, and you destroy your tool." ) );
            destroy = true;
        } else {
            who.add_msg_if_player( m_bad,
                                   _( "The lock stumps your efforts to pick it, and you damage your tool." ) );
        }
    } else {
        who.add_msg_if_player( m_bad, _( "The lock stumps your efforts to pick it." ) );
    }

    if( !perfect ) {
        // You don't gain much skill since the item does all the hard work for you
        xp_gain += std::pow( 2, who.get_skill_level( skill_mechanics ) ) + 1;
    }
    who.practice( skill_mechanics, xp_gain );

    if( !perfect
        && ( lock_roll + dice( 1, 30 ) ) > pick_roll ) {

        if( get_map().has_flag( "ALARMED", target ) ) {
            sound_event se;
            se.origin = who.abs_pos();
            se.volume = 90;
            se.category = sounds::sound_t::alarm;
            se.description = _( "an alarm sound!" );
            se.id = "environment";
            se.variant = "alarm";
            sounds::sound( se );
            if( !g->timed_events.queued( TIMED_EVENT_WANTED ) ) {
                g->timed_events.add( TIMED_EVENT_WANTED, calendar::turn + 30_minutes, 0,
                                     who.abs_sm_pos() );
            }
        } else if( veh && veh->vehicle().has_security_working() ) {
            veh->vehicle().is_alarm_on = true;
        }
    }

    if( destroy && lockpick ) {
        lockpick->detach();
    }
}

bool lockpick_activity_actor::is_pickable( mapbuffer &here, const tripoint_abs_ms &p )
{
    auto handle = abs_tile_handle::fetch( here, p );
    if( !handle ) {
        return false;
    }
    const ter_id ter_type = handle->ter();
    const furn_id furn_type = handle->furn();
    const optional_vpart_position veh = handle->vehicle_part();
    const auto door_lock = veh.part_with_feature( "DOOR_LOCKING", true );

    bool result;
    if( furn_type != f_null ) {
        result = !furn_type->lockpick_result.is_null();
    } else if( door_lock ) {
        result = door_lock.value().part().enabled;
    } else {
        result = !ter_type->lockpick_result.is_null();
    }

    return result;
}

std::optional<tripoint_abs_ms> lockpick_activity_actor::select_location( avatar &you )
{
    if( you.is_mounted() ) {
        you.add_msg_if_player( m_info, _( "You cannot do that while mounted." ) );
        return std::nullopt;
    }
    auto &here = you.get_mapbuffer();

    const std::optional<tripoint_bub_ms> target_ = choose_adjacent_highlight(
                _( "Use your lockpick where?" ), _( "There is nothing to lockpick nearby." ), [&here]
                ( const tripoint_bub_ms &p ) { return is_pickable( here, bub_to_abs( p ) ); }, false );
    if( !target_ ) {
        return std::nullopt;
    }
    auto target = bub_to_abs( *target_ );

    if( is_pickable( here, target ) ) {
        return target;
    }

    const ter_id terr_type = *here.ter( target );
    if( target == you.abs_pos() ) {
        you.add_msg_if_player( m_info, _( "You pick your nose and your sinuses swing open." ) );
    } else if( here.creature_at( target ) && here.creature_at( target )->is_npc() ) {
        you.add_msg_if_player( m_info,
                               _( "You can pick your friends, and you can pick your nose, but you can't pick your friend's nose." ) );
    } else if( !terr_type->open.is_null() ) {
        you.add_msg_if_player( m_info, _( "That door isn't locked." ) );
    } else {
        you.add_msg_if_player( m_info, _( "That cannot be picked." ) );
    }
    return std::nullopt;
}

void lockpick_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();

    jsout.member( "progress", progress );
    jsout.member( "moves_total", moves_total );
    jsout.member( "lockpick", lockpick );
    jsout.member( "fake_lockpick", fake_lockpick );
    jsout.member( "target", target );

    jsout.end_object();
}

std::unique_ptr<activity_actor> lockpick_activity_actor::deserialize( JsonIn &jsin )
{
    std::unique_ptr<lockpick_activity_actor> actor( new lockpick_activity_actor( 0,
            safe_reference<item>(), detached_ptr<item>(), tripoint_abs_ms::zero() ) );

    JsonObject data = jsin.get_object();

    data.read( "progress", actor->progress );
    data.read( "moves_total", actor->moves_total );
    data.read( "lockpick", actor->lockpick );
    data.read( "fake_lockpick", actor->fake_lockpick );
    data.read( "target", actor->target );

    return actor;
}

void oxytorch_activity_actor::start( player_activity &act, Character &who )
{
    auto &here = who.get_mapbuffer();
    auto handle = abs_tile_handle::fetch( here, target );
    if( !handle ) {
        act.set_to_null();
        return;
    }
    
    if( handle->furn() != f_null ) {
        const furn_id furn_type = handle->furn();
        if( !furn_type->oxytorch->valid() ) {
            if( !testing ) {
                debugmsg( "%s oxytorch is invalid", furn_type.id().str() );
            }
            act.set_to_null();
            return;
        }
        progress.emplace( furn_type->name(), to_moves<int>( furn_type->oxytorch->duration() ) );
    } else if( !handle->ter()->is_null() ) {
        const ter_id ter_type = handle->ter();
        if( !ter_type->oxytorch->valid() ) {
            if( !testing ) {
                debugmsg( "%s oxytorch is invalid", ter_type.id().str() );
            }
            act.set_to_null();
            return;
        }
        progress.emplace( ter_type->name(), to_moves<int>( ter_type->oxytorch->duration() ) );
    } else {
        if( !testing ) {
            debugmsg( "oxytorch activity called on invalid terrain" );
        }
        act.set_to_null();
        return;
    }
}

void oxytorch_activity_actor::do_turn( player_activity &/*act*/, Character &who )
{
    // We check available charges when first starting the cut, but this prevents abnormal behavior if torch status changes mid-activity.
    if( tool->ammo_sufficient() ) {
        tool->ammo_consume( tool->ammo_required() );
        sfx::play_activity_sound( "tool", "oxytorch", sfx::get_heard_volume( abs_to_bub( target ), 65 ) );
        if( action_time_scale::once_every_this_tick( 2_turns ) ) {
            sound_event se;
            se.origin = target;
            se.volume = 65;
            se.category = sounds::sound_t::destructive_activity;
            se.description = _( "hissssssssss!" );
            se.id = "tool";
            se.variant = "oxytorch";
            se.from_player = who.is_player();
            se.from_npc = !se.from_player;
            se.faction = who.get_faction()->id;
            se.monfaction = who.get_faction()->mon_faction;
            sounds::sound( se );
        }
    } else {
        if( who.is_avatar() ) {
            who.add_msg_if_player( m_bad, _( "Your %1$s ran out of charges." ), tool->tname() );
        } else { // who.is_npc()
            if( get_avatar().sees( who.abs_pos() ) ) {
                add_msg( _( "%1$s %2$s ran out of charges." ), who.disp_name( false,
                         true ), tool->tname() );
            }
        }
        who.cancel_activity();
    }
    if( progress.front().complete() ) {
        progress.pop();
    }
}

void oxytorch_activity_actor::finish( player_activity &act, Character &who )
{
    auto &here = who.get_mapbuffer();
    auto handle = abs_tile_handle::fetch( here, target );
    if( !handle ) {
        act.set_to_null();
        return;
    }
    const activity_data_common *data;
    
    if( handle->furn() != f_null ) {
        const furn_id furn_type = handle->furn();
        if( !furn_type->oxytorch->valid() ) {
            if( !testing ) {
                debugmsg( "%s oxytorch is invalid", furn_type.id().str() );
            }
            act.set_to_null();
            return;
        }

        const furn_str_id new_furn = furn_type->oxytorch->result();
        if( !new_furn.is_valid() ) {
            if( !testing ) {
                debugmsg( "oxytorch furniture: %s invalid furniture", new_furn.str() );
            }
            act.set_to_null();
            return;
        }

        data = static_cast<const activity_data_common *>( &*furn_type->oxytorch );
        here.set_furn( target, new_furn );
    } else if( !handle->ter()->is_null() ) {
        const ter_id ter_type = handle->ter();
        if( !ter_type->oxytorch->valid() ) {
            if( !testing ) {
                debugmsg( "%s oxytorch is invalid", ter_type.id().str() );
            }
            act.set_to_null();
            return;
        }

        const ter_str_id new_ter = ter_type->oxytorch->result();
        if( !new_ter.is_valid() ) {
            if( !testing ) {
                debugmsg( "oxytorch terrain: %s invalid terrain", new_ter.str() );
            }
            act.set_to_null();
            return;
        }

        data = static_cast<const activity_data_common *>( &*ter_type->oxytorch );
        here.set_ter( target, new_ter );
    } else {
        if( !testing ) {
            debugmsg( "oxytorch activity finished on invalid terrain" );
        }
        act.set_to_null();
        return;
    }

    for( const activity_byproduct &byproduct : data->byproducts() ) {
        const int amount = byproduct.roll();
        if( byproduct.item->count_by_charges() ) {
            here.add_item_or_charges( target, item::spawn( byproduct.item, calendar::turn, amount ) );
        } else {
            for( int i = 0; i < amount; ++i ) {
                here.add_item_or_charges( target, item::spawn( byproduct.item, calendar::turn ) );
            }
        }
    }

    // 50% chance of starting a fire.
    if( one_in( 2 ) && here.flammable_items_at( target ) ) {
        here.add_field( target, {fd_fire, 1, 10_minutes} );
    }

    if( !data->message().empty() ) {
        who.add_msg_if_player( m_info, data->message().translated() );
    }

    act.set_to_null();
}

void oxytorch_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();
    jsout.member( "progress", progress );
    jsout.member( "target", target );
    jsout.member( "tool", tool );
    jsout.end_object();
}

std::unique_ptr<activity_actor> oxytorch_activity_actor::deserialize( JsonIn &jsin )
{
    std::unique_ptr<oxytorch_activity_actor> actor( new oxytorch_activity_actor(
                tripoint_abs_ms::zero(), safe_reference<item>() ) );
    JsonObject data = jsin.get_object();
    data.read( "progress", actor->progress );
    data.read( "target", actor->target );
    data.read( "tool", actor->tool );
    return actor;
}

void migration_cancel_activity_actor::do_turn( player_activity &act, Character &who )
{
    // Stop the activity
    act.set_to_null();

    // Ensure that neither avatars nor npcs end up in an invalid state
    if( who.is_npc() ) {
        npc &npc_who = dynamic_cast<npc &>( who );
        npc_who.revert_after_activity();
    } else {
        avatar &avatar_who = dynamic_cast<avatar &>( who );
        avatar_who.clear_destination();
        avatar_who.backlog.clear();
    }
}

void migration_cancel_activity_actor::serialize( JsonOut &jsout ) const
{
    // This will probably never be called, but write null to avoid invalid json in
    // the case that it is
    jsout.write_null();
}

std::unique_ptr<activity_actor> migration_cancel_activity_actor::deserialize( JsonIn & )
{
    return std::unique_ptr<migration_cancel_activity_actor>();
}

void toggle_gate_activity_actor::start( player_activity &, Character & )
{
    progress.emplace( "gate", moves_total );
}

void toggle_gate_activity_actor::do_turn( player_activity &, Character & )
{
    if( progress.front().complete() ) {
        progress.pop();
        return;
    }
}

void toggle_gate_activity_actor::finish( player_activity &act, Character &who )
{
    gates::toggle_gate( who.get_mapbuffer(), placement );
    act.set_to_null();
}

void toggle_gate_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();

    jsout.member( "progress", progress );
    jsout.member( "moves", moves_total );
    jsout.member( "placement", placement );

    jsout.end_object();
}

std::unique_ptr<activity_actor> toggle_gate_activity_actor::deserialize( JsonIn &jsin )
{
    std::unique_ptr<toggle_gate_activity_actor> actor( new toggle_gate_activity_actor( 0,
            tripoint_abs_ms::zero() ) );

    JsonObject data = jsin.get_object();

    data.read( "progress", actor->progress );
    data.read( "moves", actor->moves_total );
    data.read( "placement", actor->placement );

    return actor;
}


stash_activity_actor::stash_activity_actor( Character &ch, const drop_locations &items,
        const tripoint_rel_ms &relpos ) : relpos( relpos )
{
    this->items = pickup::reorder_for_dropping( ch, items );
}

void stash_activity_actor::start( player_activity &, Character & )
{
    // Dummy progress task to indicate ongoing activity
    progress.dummy();
}

void stash_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();

    jsout.member( "progress", progress );
    jsout.member( "items", items );
    jsout.member( "relpos", relpos );

    jsout.end_object();
}

std::unique_ptr<activity_actor> stash_activity_actor::deserialize( JsonIn &jsin )
{
    std::unique_ptr<stash_activity_actor> actor( new stash_activity_actor() );

    JsonObject data = jsin.get_object();

    data.read( "progress", actor->progress );
    data.read( "items", actor->items );
    data.read( "relpos", actor->relpos );

    return actor;
}

void throw_activity_actor::do_turn( player_activity &act, Character &who )
{
    // Make copies of relevant values since the class would
    // not be available after act.set_to_null()
    if( !target ) {
        debugmsg( "Lost weapon while throwing" );
        act.set_to_null();
        return;
    }

    item *it = &*target;
    std::optional<tripoint_abs_ms> blind_throw_pos = blind_throw_from_pos;

    // Stop the activity. Whether we will or will not throw doesn't matter.
    act.set_to_null();
    if( !who.is_avatar() ) {
        // Sanity check
        debugmsg( "ACT_THROW is not applicable for NPCs." );
        return;
    }

    // Shift our position to our peeking position so the target UI can see from there.
    const auto original_player_position = who.abs_pos();
    if( blind_throw_pos ) {
        who.setpos( *blind_throw_pos );
    }

    target_handler::trajectory trajectory = target_handler::mode_throw( *who.as_avatar(), *it,
                                            blind_throw_pos.has_value() );

    // If we previously shifted our position, put ourselves back now that we've picked our target.
    if( blind_throw_pos ) {
        who.setpos( original_player_position );
    }

    if( trajectory.empty() ) {
        return;
    }

    if( it != &who.primary_weapon() ) {
        // This is to represent "implicit offhand wielding"
        int extra_cost = who.item_handling_cost( *it, true, INVENTORY_HANDLING_PENALTY / 2 );
        who.mod_moves( -extra_cost );
    }
    detached_ptr<item> det = target->split( 1 );
    ranged::throw_item( who, trajectory.back(), std::move( det ), blind_throw_pos );
}

void throw_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();

    jsout.member( "progress", progress );
    jsout.member( "target_loc", target );
    jsout.member( "blind_throw_from_pos", blind_throw_from_pos );

    jsout.end_object();
}

std::unique_ptr<activity_actor> throw_activity_actor::deserialize( JsonIn &jsin )
{
    std::unique_ptr<throw_activity_actor> actor( new throw_activity_actor() );

    JsonObject data = jsin.get_object();

    data.read( "progress", actor->progress );
    data.read( "target_loc", actor->target );
    data.read( "blind_throw_from_pos", actor->blind_throw_from_pos );

    return actor;
}


// ---- craft_activity_actor ----

craft_activity_actor::craft_activity_actor(
    const recipe *rec,
    int batch_size,
    int craft_counter,
    const tripoint_abs_ms &location,
    bench_type bench,
    int tools_mult_percent,
    const tripoint_abs_ms &bench_pos,
    std::vector<comp_selection<item_comp>> item_selections,
    std::vector<comp_selection<tool_comp>> tool_selections,
    bool tools_prepaid,
    bool is_long
) : rec( rec ), batch_size( batch_size ), craft_counter( craft_counter ),
    location( location ),
    bench( bench ),
    tools_mult_percent( tools_mult_percent ),
    bench_pos( bench_pos ),
    item_selections( std::move( item_selections ) ),
    tool_selections( std::move( tool_selections ) ),
    tools_prepaid( tools_prepaid ),
    is_long( is_long ),
    is_valid( rec != nullptr )
{}

auto craft_activity_actor::find_in_progress_craft( const player_activity &act,
        Character &who ) const -> item * // *NOPAD*
{
    if( !act.targets.empty() && act.targets.front() && act.targets.front()->is_craft() &&
        &act.targets.front()->get_making() == rec ) {
        return &*act.targets.front();
    }

    item *result = nullptr;
    who.visit_items( [&]( item * it ) {
        if( it->is_craft() && &it->get_making() == rec ) {
            result = it;
            return VisitResponse::ABORT;
        }
        return VisitResponse::NEXT;
    } );
    if( result ) {
        return result;
    }
    // If not in inventory, check the map at the crafter's feet — set_item_inventory
    // may have placed it there if the NPC was over their carry capacity.
    map_selector sel( who.bub_pos(), 0 );
    sel.visit_items( [&]( item * it ) {
        if( it->is_craft() && &it->get_making() == rec ) {
            result = it;
            return VisitResponse::ABORT;
        }
        return VisitResponse::NEXT;
    } );
    return result;
}

void craft_activity_actor::calc_all_moves( player_activity &act, Character &who )
{
    if( !rec || !is_valid ) {
        act.set_to_null();
        return;
    }

    const int current_turn = to_turn<int>( calendar::turn );

    // Catch-up: apply time elapsed while NPC was outside the reality bubble.
    // last_turn_nr >= 0 means start() already ran in a previous session.
    if( last_turn_nr >= 0 && current_turn > last_turn_nr ) {
        item *craft_item = find_in_progress_craft( act, who );
        if( craft_item ) {
            const int elapsed_turns = current_turn - last_turn_nr;
            const double base_total_moves = std::max( 1, rec->batch_time( batch_size, 1.0f, 0 ) );
            // No live crafting modifiers are applied while outside the reality bubble.
            const auto moves_elapsed = action_time_scale::activity_progress_for_turns( elapsed_turns );
            const int old_counter = craft_item->get_counter();
            const int new_counter = std::min(
                                        static_cast<int>( old_counter + moves_elapsed / base_total_moves * 10'000'000.0 ),
                                        10'000'000 );
            craft_item->set_counter( new_counter );
            craft_counter = new_counter;

            const int five_percent_steps = new_counter / 500'000 - old_counter / 500'000;
            if( five_percent_steps > 0 ) {
                who.craft_skill_gain( *craft_item, five_percent_steps );
            }

            // Re-build progress counter to match updated craft state
            const int remaining = std::max( 0, static_cast<int>(
                                                base_total_moves * ( 1.0 - new_counter / 10'000'000.0 ) ) );
            if( !activity_actor::progress.empty() ) {
                activity_actor::progress.mod_moves_left(
                    remaining - activity_actor::progress.get_moves_left() );
            } else {
                activity_actor::progress.emplace( craft_item->tname(),
                                                  static_cast<int>( base_total_moves ), remaining );
            }

            if( new_counter >= 10'000'000 ) {
                // Drain so complete() fires on the next do_turn check
                activity_actor::progress.mod_moves_left( -activity_actor::progress.get_moves_left() );
            }
        }
    }

    last_turn_nr = current_turn;

    // Re-build progress counter after deserialization if catch-up didn't already do it
    if( activity_actor::progress.empty() ) {
        item *craft_item = find_in_progress_craft( act, who );
        const std::string name = craft_item ? craft_item->tname() : rec->result_name();
        const int base_total = std::max( 1, rec->batch_time( batch_size, 1.0f, 0 ) );
        const int remaining = std::max( 1, static_cast<int>(
                                            base_total * ( 1.0 - craft_counter / 10'000'000.0 ) ) );
        activity_actor::progress.emplace( name, base_total, remaining );
    }

    item *craft_item = find_in_progress_craft( act, who );
    if( craft_item ) {
        refresh_speed( act, who, *craft_item );
    }
}

void craft_activity_actor::refresh_speed( player_activity &act, const Character &who,
        const item &craft_item, std::optional<bench_location> bench ) const
{
    const bench_location resolved_bench = bench ? *bench : find_best_bench( who, craft_item );
    const recipe &making = *rec;
    const float tools_mult = cached_tools_mult != 0.0f ? cached_tools_mult
                             : crafting_tools_speed_multiplier( who, making );
    act.speed.light        = lighting_crafting_speed_multiplier( who, making );
    act.speed.bench_factor = workbench_crafting_speed_multiplier( craft_item, resolved_bench );
    act.speed.morale       = morale_crafting_speed_multiplier( who, making );
    act.speed.tools        = tools_mult;
    act.speed.player_speed = who.get_speed() / 100.0f;
    const int assistants   = who.available_assistant_count( making );
    if( assistants > 0 ) {
        const double base_no_assist   = std::max( 1, making.batch_time( batch_size, 1.0f, 0 ) );
        const double base_with_assist = std::max( 1, making.batch_time( batch_size, 1.0f, assistants ) );
        act.speed.assist = static_cast<float>( base_no_assist / base_with_assist );
    } else {
        act.speed.assist = 1.0f;
    }
    // Mutation and game-option multipliers have no dedicated speed field; fold them
    // into skills so act.speed.total() matches the actual crafting rate.
    const float mutation_mult = who.mutation_value( "crafting_speed_modifier" );
    const float game_opt_mult = get_option<int>( "CRAFTING_SPEED_MULT" ) == 0
                                ? 9999.0f
                                : 100.0f / static_cast<float>( get_option<int>( "CRAFTING_SPEED_MULT" ) );
    act.speed.skills = mutation_mult * game_opt_mult;
}

void craft_activity_actor::start( player_activity &act, Character &who )
{
    if( !rec || !is_valid ) {
        act.set_to_null();
        return;
    }

    item *craft_item = find_in_progress_craft( act, who );
    if( !craft_item ) {
        who.add_msg_player_or_npc(
            _( "You lost your in progress %s and had to stop crafting." ),
            _( "<npcname> lost the in progress %s and had to stop crafting." ),
            rec->result_name() );
        act.set_to_null();
        return;
    }

    cached_tools_mult = crafting_tools_speed_multiplier( who, *rec );
    craft_counter = craft_item->get_counter();
    last_turn_nr = to_turn<int>( calendar::turn );  // mark fresh start so calc_all_moves skips catch-up
    const int base_total = std::max( 1, rec->batch_time( batch_size, 1.0f, 0 ) );
    const int remaining = craft_counter == 0
                          ? base_total
                          : std::max( 1, static_cast<int>( base_total * ( 1.0 - craft_counter / 10'000'000.0 ) ) );
    activity_actor::progress.emplace( craft_item->tname(), base_total, remaining );
}

void craft_activity_actor::do_turn( player_activity &act, Character &who )
{
    if( !rec || !is_valid ) {
        act.set_to_null();
        return;
    }

    item *craft_item = find_in_progress_craft( act, who );
    if( !craft_item ) {
        who.add_msg_player_or_npc(
            _( "You no longer have the in progress craft in your possession.  "
               "You stop crafting.  "
               "Reactivate the in progress craft to continue crafting." ),
            _( "<npcname> no longer has the in progress craft in their possession.  "
               "<npcname> stops crafting." ) );
        act.set_to_null();
        return;
    }

    const recipe &making = *rec;
    if( cached_tools_mult == 0.0f ) {
        cached_tools_mult = crafting_tools_speed_multiplier( who, making );
    }
    const bench_location bench = find_best_bench( who, *craft_item );
    refresh_speed( act, who, *craft_item, bench );
    const float crafting_speed = crafting_speed_multiplier( who, *craft_item, bench, act.speed.tools );
    const int assistants = who.available_assistant_count( making );

    if( crafting_speed <= 0.0f ) {
        who.add_msg_player_or_npc( m_bad,
                                   _( "You cannot continue crafting." ),
                                   _( "<npcname> cannot continue crafting." ) );
        act.set_to_null();
        return;
    }

    const int old_counter = craft_item->get_counter();
    const double base_total_moves = std::max( 1, making.batch_time( batch_size, 1.0f, 0 ) );
    const double cur_total_moves = std::max( 1, making.batch_time( batch_size, crafting_speed,
                                   assistants ) );
    const auto scaled_moves = action_time_scale::activity_progress_from_actor_moves( who );
    const auto delta_progress = scaled_moves * base_total_moves / cur_total_moves;
    const double current_progress = old_counter * base_total_moves / 10'000'000.0 + delta_progress;
    const int new_counter = std::min(
                                static_cast<int>( std::round( current_progress / base_total_moves * 10'000'000.0 ) ),
                                10'000'000 );
    const int five_percent_steps = new_counter / 500'000 - old_counter / 500'000;
    craft_item->set_counter( new_counter );
    craft_counter = new_counter;

    who.set_moves( 0 );

    if( five_percent_steps > 0 ) {
        who.craft_skill_gain( *craft_item, five_percent_steps );

        if( !tools_prepaid && !who.craft_consume_tools( *craft_item, five_percent_steps, false ) ) {
            act.set_to_null();
            return;
        }
    }

    // Keep the progress_counter in sync so the UI shows correct values
    if( !activity_actor::progress.empty() ) {
        const int new_moves_left = static_cast<int>(
                                       base_total_moves * ( 1.0 - static_cast<double>( new_counter ) / 10'000'000.0 ) );
        const int delta = new_moves_left - activity_actor::progress.get_moves_left();
        if( delta != 0 ) {
            activity_actor::progress.mod_moves_left( delta );
        }
    }

    last_turn_nr = to_turn<int>( calendar::turn );

    if( new_counter >= 10'000'000 ) {
        // Signal completion so player_activity::do_turn calls finish()
        if( !activity_actor::progress.empty() ) {
            activity_actor::progress.mod_moves_left( -activity_actor::progress.get_moves_left() );
        }
    } else if( new_counter >= craft_item->get_next_failure_point() ) {
        const bool destroy = craft_item->handle_craft_failure( who );
        if( destroy ) {
            who.add_msg_player_or_npc(
                _( "There is nothing left of the %s to craft from." ),
                _( "There is nothing left of the %s <npcname> was crafting." ),
                craft_item->tname() );
            craft_item->detach();
            act.set_to_null();
        }
        // If !destroy, handle_craft_failure may have called cancel_activity already
    }
}

void craft_activity_actor::finish( player_activity &act, Character &who )
{
    act.set_to_null();
    do_complete_craft( act, who );
}

void craft_activity_actor::do_complete_craft( player_activity &act, Character &who )
{
    item *craft_item = find_in_progress_craft( act, who );
    if( !craft_item ) {
        debugmsg( "craft_activity_actor::do_complete_craft: no craft item found for %s",
                  rec ? rec->result_name() : "unknown" );
        return;
    }
    ::complete_craft( who, *craft_item );
    craft_item->detach();
    if( is_long && rec ) {
        if( who.making_would_work( rec->ident(), batch_size ) ) {
            who.last_craft->execute( abs_to_bub( location ) );
        }
    }
}

act_progress_message craft_activity_actor::get_progress_message(
    const player_activity &act, const Character &who ) const
{
    if( !rec || !is_valid ) {
        return act_progress_message::make_empty();
    }

    const int assistants = who.available_assistant_count( *rec );
    const double base_total_moves = std::max( 1, rec->batch_time( batch_size, 1.0f, 0 ) );
    const double remaining_pct = 1.0 - craft_counter / 10'000'000.0;
    const auto total_mult = act.speed.total();
    const auto remaining_moves = static_cast<int>( std::ceil( remaining_pct * base_total_moves ) );
    const auto remaining_turns = action_time_scale::turns_for_progress( remaining_moves,
                                 act.speed.calendar_moves_per_turn() );

    const std::string time_desc = string_format( _( "Time left: %s" ),
                                  to_string( time_duration::from_turns( remaining_turns ) ) );

    const auto fmt_spd = [&]( float level, const std::string & name ) -> std::string {
        const int pct = static_cast<int>( level * 100 );
        if( pct == 100 )
        {
            return "";
        }
        nc_color col = pct > 100 ? c_green : c_red;
        return string_format( " - %s: %s\n", name,
                              colorize( std::to_string( pct ) + '%', col ) );
    };

    std::string mults_desc = _( "Crafting speed multipliers:\n" );
    const int total_pct = static_cast<int>( total_mult * 100 );
    nc_color total_col = total_pct > 100 ? c_green : c_red;
    mults_desc += string_format( " - %s: %s\n", _( "Total" ),
                                 colorize( std::to_string( total_pct ) + '%', total_col ) );
    mults_desc += fmt_spd( act.speed.player_speed, _( "Speed" ) );
    mults_desc += fmt_spd( act.speed.light, _( "Light" ) );
    mults_desc += fmt_spd( act.speed.bench_factor, _( "Workbench" ) );
    mults_desc += fmt_spd( act.speed.morale, _( "Morale" ) );
    mults_desc += fmt_spd( act.speed.tools, _( "Tools" ) );
    if( assistants > 0 ) {
        mults_desc += fmt_spd( act.speed.assist, _( "Assistants" ) );
    }

    return act_progress_message::make_full(
               string_format( _( "%s: %s\n\n%s\n\n%s" ),
                              act.get_verb().translated(), rec->result_name(),
                              time_desc, mults_desc ) );
}

void craft_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();
    jsout.member( "progress", activity_actor::progress );
    jsout.member( "recipe", rec ? rec->ident().str() : std::string() );
    jsout.member( "batch_size", batch_size );
    jsout.member( "craft_counter", craft_counter );
    jsout.member( "location", location );
    jsout.member( "bench", static_cast<int>( bench ) );
    jsout.member( "tools_mult_percent", tools_mult_percent );
    jsout.member( "bench_pos", bench_pos );
    jsout.member( "item_selections", item_selections );
    jsout.member( "tool_selections", tool_selections );
    jsout.member( "tools_prepaid", tools_prepaid );
    jsout.member( "is_long", is_long );
    jsout.member( "last_turn_nr", last_turn_nr );
    jsout.end_object();
}

std::unique_ptr<activity_actor> craft_activity_actor::deserialize( JsonIn &jsin )
{
    auto actor = std::make_unique<craft_activity_actor>();
    JsonObject data = jsin.get_object();

    data.read( "progress", actor->activity_actor::progress );
    std::string recipe_str;
    data.read( "recipe", recipe_str );
    if( !recipe_str.empty() ) {
        const recipe_id rid( recipe_str );
        if( rid.is_valid() ) {
            actor->rec = &*rid;
            actor->is_valid = true;
        }
    }
    data.read( "batch_size", actor->batch_size );
    data.read( "craft_counter", actor->craft_counter );
    data.read( "location", actor->location );

    int bt = 0;
    data.read( "bench", bt );
    actor->bench = static_cast<bench_type>( bt );
    data.read( "tools_mult_percent", actor->tools_mult_percent );
    data.read( "bench_pos", actor->bench_pos );

    data.read( "item_selections", actor->item_selections );
    data.read( "tool_selections", actor->tool_selections );
    data.read( "tools_prepaid", actor->tools_prepaid );
    data.read( "is_long", actor->is_long );
    data.read( "last_turn_nr", actor->last_turn_nr );

    return actor;
}

std::unique_ptr<activity_actor> craft_activity_actor::legacy_deserialize( const JsonObject &data )
{
    auto actor = std::make_unique<craft_activity_actor>();

    // bench type from values[1]
    auto values = data.get_int_array( "values" );
    if( values.size() >= 2 ) {
        actor->bench = static_cast<bench_type>( values[1] );
    }
    // tools_mult_percent from values[2] (optional)
    if( values.size() >= 3 ) {
        actor->tools_mult_percent = values[2];
    }
    // bench_pos from coords[0]
    auto coords = std::vector<tripoint_abs_ms>();
    data.read( "coords", coords );
    if( !coords.empty() ) {
        actor->bench_pos = coords[0];
    }

    // Note: rec, batch_size, location, item_selections, tool_selections, etc.
    // are set by the existing ACT_CRAFT creation path via do_activity in crafting.cpp.
    // Legacy saves have them in the player_activity fields which get read separately.
    // This only handles the fields we're migrating INTO the actor.

    return actor;
}

inline void construction_activity_actor::calc_all_moves( player_activity &act, Character &who )
{
    // Check if pc was lost for some reason, but actually still exists on map, e.g. save/load
    if( !pc ) {
        map &here = get_map();
        auto local = abs_to_bub( target );
        pc = here.partial_con_at( tripoint_bub_ms( local ) );
    }
    //if something goes terribly wrong we don't CTD
    if( !pc ) {
        act.set_to_null();
        return;
    }
    auto reqs = activity_reqs_adapter( *pc->id );
    act.speed.calc_all_moves( who, reqs );
}

void construction_activity_actor::start( player_activity &/*act*/, Character &/*who*/ )
{
    map &here = get_map();
    auto local = abs_to_bub( target );
    pc = here.partial_con_at( tripoint_bub_ms( local ) );
    auto &built = *pc->id;

    std::string name;

    if( pc->id == deconstruct || pc->id == deconstruct_simple ||
        built.group == advanced_object_deconstruction ) {
        if( here.has_furn( local ) ) {
            const furn_id furn_type = here.furn( local );
            name = furn_type->name();
        } else if( !here.ter( local )->is_null() ) {
            const ter_id ter_type = here.ter( local );
            name = ter_type->name();
        }
    } else {
        name = built.post_furniture.is_empty()
               ? ""
               : built.post_furniture->name();
        name = built.post_terrain.is_empty()
               ? name
               : built.post_terrain->name();
    }

    int total_time = std::max( 1, built.adjusted_time() );
    int left = pc->counter == 0
               ? total_time
               : total_time - pc->counter / 10'000'000.0 * total_time;

    progress.emplace( name, total_time, left );
}

void construction_activity_actor::do_turn( player_activity &act, Character &who )
{
    // Check if pc was lost for some reason, but actually still exists on map, e.g. save/load
    if( !pc ) {
        map &here = get_map();
        auto local = abs_to_bub( target );
        pc = here.partial_con_at( tripoint_bub_ms( local ) );
    }

    // Maybe the player and the NPC are working on the same construction at the same time or toubles during load
    if( !pc ) {
        act.set_to_null();
        add_msg( m_info, _( "%s did not find an unfinished construction at the activity spot." ),
                 who.disp_name() );
        return;
    }

    pc->counter = progress.front().to_counter();

    if( progress.front().complete() ) {
        progress.pop();
        return;
    } else {
        auto &built = *pc->id;
        if( !who.has_trait( trait_DEBUG_HS ) && !who.meets_skill_requirements( built ) ) {
            add_msg( m_info, _( "%s can't work on this construction anymore." ), who.disp_name() );
            act.set_to_null();
            return;
        }
    }
}

void construction_activity_actor::finish( player_activity &act, Character &who )
{
    complete_construction( who, target );
    act.set_to_null();
}

void construction_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();
    jsout.member( "progress", progress );
    jsout.member( "target", target );
    jsout.end_object();
}

std::unique_ptr<activity_actor> construction_activity_actor::deserialize( JsonIn &jsin )
{
    std::unique_ptr<construction_activity_actor> actor( new construction_activity_actor(
                tripoint_abs_ms( tripoint_zero ) ) );
    JsonObject data = jsin.get_object();
    data.read( "progress", actor->progress );
    data.read( "target", actor->target );
    return actor;
}

void assist_activity_actor::start( player_activity &/*act*/, Character &/*who*/ )
{
    progress.dummy();
}

void assist_activity_actor::serialize( JsonOut &jsout ) const
{
    // Activity is not being saved but still provide some valid json if called.
    jsout.write_null();
}

std::unique_ptr<activity_actor> assist_activity_actor::deserialize( JsonIn & )
{
    return std::make_unique<assist_activity_actor>();
}

std::unique_ptr<activity_actor> salvage_activity_actor::deserialize( JsonIn &jsin )
{
    std::unique_ptr<salvage_activity_actor> actor( new salvage_activity_actor() );

    JsonObject data = jsin.get_object();

    data.read( "progress", actor->progress );
    data.read( "targets", actor->targets );
    data.read( "pos", actor->pos );
    data.read( "mute_prompts", actor->mute_prompts );

    return actor;
}

// ─────────────────────────────────────────────────────────────────────────────
// liquid_transfer_actor (ACT_FILL_LIQUID)
// ─────────────────────────────────────────────────────────────────────────────

liquid_transfer_actor::liquid_transfer_actor(
    liquid_source_type src_type,
    const tripoint_abs_ms &src_pos,
    int src_part_index,
    liquid_target_type tgt_type,
    const tripoint_abs_ms &tgt_pos,
    safe_reference<item> tgt_container
) : source_type( src_type )
  , source_pos( src_pos )
  , source_part_index( src_part_index )
  , target_type( tgt_type )
  , target_pos( tgt_pos )
  , target_container( std::move( tgt_container ) )
{}

void liquid_transfer_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();
    jsout.member( "progress", activity_actor::progress );
    jsout.member( "source_type", static_cast<int>( source_type ) );
    jsout.member( "source_pos", source_pos );
    jsout.member( "source_part_index", source_part_index );
    jsout.member( "target_type", static_cast<int>( target_type ) );
    jsout.member( "target_pos", target_pos );
    jsout.member( "target_container", target_container );
    jsout.end_object();
}

std::unique_ptr<activity_actor> liquid_transfer_actor::deserialize( JsonIn &jsin )
{
    auto actor = std::make_unique<liquid_transfer_actor>();
    JsonObject data = jsin.get_object();
    data.read( "progress", actor->activity_actor::progress );

    int st = 0;
    data.read( "source_type", st );
    actor->source_type = static_cast<liquid_source_type>( st );

    data.read( "source_pos", actor->source_pos );
    data.read( "source_part_index", actor->source_part_index );

    int tt = 0;
    data.read( "target_type", tt );
    actor->target_type = static_cast<liquid_target_type>( tt );

    data.read( "target_pos", actor->target_pos );
    data.read( "target_container", actor->target_container );

    return actor;
}

std::unique_ptr<activity_actor> liquid_transfer_actor::legacy_deserialize( const JsonObject &data )
{
    auto actor = std::make_unique<liquid_transfer_actor>();

    auto values = data.get_int_array( "values" );
    if( values.size() >= 1 ) {
        actor->source_type = static_cast<liquid_source_type>( values[0] );
    }
    if( values.size() >= 2 ) {
        actor->source_part_index = values[1];
    }
    if( values.size() >= 3 ) {
        actor->target_type = static_cast<liquid_target_type>( values[2] );
    }

    auto coords = std::vector<tripoint_abs_ms>();
    data.read( "coords", coords );
    if( coords.size() >= 1 ) {
        actor->source_pos = coords[0];
    }
    if( coords.size() >= 2 ) {
        actor->target_pos = coords[1];
    }

    auto targets = std::vector<safe_reference<item>>();
    data.read( "targets", targets );
    if( !targets.empty() ) {
        actor->target_container = std::move( targets[0] );
    }

    return actor;
}

void liquid_transfer_actor::start( player_activity &act, Character &who ) {}
void liquid_transfer_actor::do_turn( player_activity &act, Character &who ) {}
void liquid_transfer_actor::finish( player_activity &act, Character &who ) {}

// ─────────────────────────────────────────────────────────────────────────────
// vehicle_work_actor (ACT_VEHICLE)
// ─────────────────────────────────────────────────────────────────────────────

vehicle_work_actor::vehicle_work_actor(
    char cmd,
    const tripoint_abs_ms &ppos,
    const tripoint_mnt_veh &cmount,
    const vpart_id &ptype,
    int pindex,
    const std::unordered_set<tripoint_abs_ms> &vpoints
) : command( cmd )
  , part_pos( ppos )
  , cursor_mount( cmount )
  , part_type( ptype )
  , part_index( pindex )
  , vehicle_points( vpoints )
{}

void vehicle_work_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();
    jsout.member( "progress", activity_actor::progress );
    jsout.member( "command", static_cast<int>( command ) );
    jsout.member( "part_pos", part_pos );
    jsout.member( "cursor_mount", cursor_mount );
    jsout.member( "part_type", part_type );
    jsout.member( "part_index", part_index );
    jsout.member( "vehicle_points", vehicle_points );
    jsout.end_object();
}

std::unique_ptr<activity_actor> vehicle_work_actor::deserialize( JsonIn &jsin )
{
    auto actor = std::make_unique<vehicle_work_actor>();
    JsonObject data = jsin.get_object();
    data.read( "progress", actor->activity_actor::progress );

    int cmd = 0;
    data.read( "command", cmd );
    actor->command = static_cast<char>( cmd );

    data.read( "part_pos", actor->part_pos );
    data.read( "cursor_mount", actor->cursor_mount );
    data.read( "part_type", actor->part_type );
    data.read( "part_index", actor->part_index );
    data.read( "vehicle_points", actor->vehicle_points );

    return actor;
}

std::unique_ptr<activity_actor> vehicle_work_actor::legacy_deserialize( const JsonObject &data )
{
    auto values = data.get_int_array( "values" );

    // Need at least 7 values for either format
    if( values.size() < 7 ) {
        return nullptr;
    }

    const bool very_old = values.size() == 8;

    // Part position — values[0], values[1] are bubble in very old format, abs otherwise
    tripoint_abs_ms part_pos;
    if( very_old ) {
        part_pos = bub_to_abs( tripoint_bub_ms( values[0], values[1], values[7] ) );
    } else {
        part_pos = tripoint_abs_ms( values[0], values[1], values[2] );
    }

    // Cursor mount
    tripoint_mnt_veh cursor_mount{ 0, 0, 0 };
    if( very_old ) {
        cursor_mount = tripoint_mnt_veh( -values[4], -values[5], 0 );
    } else {
        cursor_mount = tripoint_mnt_veh( values[3], values[4], values[5] );
    }

    int part_index = values[6];

    // Part type from str_values
    auto str_values = data.get_string_array( "str_values" );
    vpart_id part_type;
    if( !str_values.empty() ) {
        part_type = vpart_id( str_values[0] );
    }

    // Command from index
    char command = static_cast<char>( data.get_int( "index" ) );

    // Vehicle points from coord_set
    auto vehicle_points = std::unordered_set<tripoint_abs_ms>();
    data.read( "coord_set", vehicle_points );

    return std::make_unique<vehicle_work_actor>(
               command, part_pos, cursor_mount, part_type, part_index, vehicle_points );
}

void vehicle_work_actor::start( player_activity &act, Character &who ) {}
void vehicle_work_actor::do_turn( player_activity &act, Character &who ) {}
void vehicle_work_actor::finish( player_activity &act, Character &who ) {}

// ─────────────────────────────────────────────────────────────────────────────
// repair_actor (ACT_REPAIR_ITEM)
// ─────────────────────────────────────────────────────────────────────────────

repair_actor::repair_actor(
    hack_type_t htype,
    const tripoint_abs_ms &tpos,
    const itype_id &ttool,
    int cpart_idx
) : hack_type( htype )
  , target_pos( tpos )
  , tool_type( ttool )
  , crafter_part_index( cpart_idx )
{}

void repair_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();
    jsout.member( "progress", activity_actor::progress );
    jsout.member( "hack_type", static_cast<int>( hack_type ) );
    jsout.member( "target_pos", target_pos );
    jsout.member( "tool_type", tool_type );
    jsout.member( "crafter_part_index", crafter_part_index );
    jsout.end_object();
}

std::unique_ptr<activity_actor> repair_actor::deserialize( JsonIn &jsin )
{
    auto data = jsin.get_object();
    auto actor = std::make_unique<repair_actor>();
    data.read( "progress", actor->activity_actor::progress );
    if( data.has_member( "hack_type" ) ) {
        actor->is_hack = true;
        int ht = 0; data.read( "hack_type", ht );
        actor->hack_type = static_cast<hack_type_t>( ht );
        data.read( "target_pos", actor->target_pos );
        data.read( "tool_type", actor->tool_type );
        data.read( "crafter_part_index", actor->crafter_part_index );
    } else {
        actor->is_hack = false;
        data.read( "iuse_name", actor->iuse_name );
        data.read( "item_pos", actor->item_pos );
        data.read( "tool", actor->tool );
    }
    return actor;
}

std::unique_ptr<activity_actor> repair_actor::legacy_deserialize( const JsonObject &data )
{
    // Determine if hack path: has values[2] (hack_type) set to non-zero
    auto values = data.get_int_array( "values" );
    if( values.size() >= 3 && values[2] != 0 ) {
        // Hack path
        auto actor = std::make_unique<repair_actor>();
        actor->is_hack = true;
        if( values.size() >= 2 ) actor->crafter_part_index = values[1];
        actor->hack_type = static_cast<hack_type_t>( values[2] );

        // tool_type from str_values[1]
        auto str_values = data.get_string_array( "str_values" );
        if( str_values.size() >= 2 ) actor->tool_type = itype_id( str_values[1] );

        // target_pos from coords[0]
        auto coords = std::vector<tripoint_abs_ms>();
        data.read( "coords", coords );
        if( !coords.empty() ) actor->target_pos = coords[0];
        return actor;
    }

    // Core path
    auto actor = std::make_unique<repair_actor>();
    actor->is_hack = false;
    auto str_values = data.get_string_array( "str_values" );
    if( !str_values.empty() ) actor->iuse_name = str_values[0];
    auto targets = std::vector<safe_reference<item>>();
    data.read( "targets", targets );
    if( !targets.empty() ) actor->tool = std::move( targets[0] );
    actor->item_pos = data.get_int( "index", -1 );
    return actor;
}

void repair_actor::start( player_activity &act, Character &who ) {}
void repair_actor::do_turn( player_activity &act, Character &who ) {}
void repair_actor::finish( player_activity &act, Character &who ) {}

// ─────────────────────────────────────────────────────────────────────────────
// wear_actor (ACT_WEAR)
// ─────────────────────────────────────────────────────────────────────────────

wear_actor::wear_actor( std::vector<wear_target> targets )
    : to_wear( std::move( targets ) )
{}

void wear_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();
    jsout.member( "progress", activity_actor::progress );
    jsout.member( "to_wear" );
    jsout.start_array();
    for( const auto &wt : to_wear ) {
        jsout.start_object();
        jsout.member( "item", wt.item_ref );
        jsout.member( "quantity", wt.quantity );
        jsout.end_object();
    }
    jsout.end_array();
    jsout.end_object();
}

std::unique_ptr<activity_actor> wear_actor::deserialize( JsonIn &jsin )
{
    auto actor = std::make_unique<wear_actor>();
    JsonObject data = jsin.get_object();
    data.read( "progress", actor->activity_actor::progress );

    JsonArray arr = data.get_array( "to_wear" );
    for( JsonObject wt : arr ) {
        wear_actor::wear_target target;
        wt.read( "item", target.item_ref );
        wt.read( "quantity", target.quantity );
        actor->to_wear.push_back( std::move( target ) );
    }

    return actor;
}

std::unique_ptr<activity_actor> wear_actor::legacy_deserialize( const JsonObject &data )
{
    auto actor = std::make_unique<wear_actor>();

    auto targets_vec = std::vector<safe_reference<item>>();
    data.read( "targets", targets_vec );

    auto values = data.get_int_array( "values" );

    size_t count = std::min( targets_vec.size(), values.size() );
    for( size_t i = 0; i < count; i++ ) {
        actor->to_wear.push_back( {
            .item_ref = std::move( targets_vec[i] ),
            .quantity = values[i]
        } );
    }

    return actor;
}

void wear_actor::start( player_activity &act, Character &who ) {}
void wear_actor::do_turn( player_activity &act, Character &who ) {}
void wear_actor::finish( player_activity &act, Character &who ) {}

// ─────────────────────────────────────────────────────────────────────────────
// wait_stamina_actor (ACT_WAIT_STAMINA)
// ─────────────────────────────────────────────────────────────────────────────

wait_stamina_actor::wait_stamina_actor( int threshold )
    : stamina_threshold( threshold )
{}

void wait_stamina_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();
    jsout.member( "progress", activity_actor::progress );
    jsout.member( "stamina_threshold", stamina_threshold );
    jsout.member( "stamina_initial", stamina_initial );
    jsout.end_object();
}

std::unique_ptr<activity_actor> wait_stamina_actor::deserialize( JsonIn &jsin )
{
    auto actor = std::make_unique<wait_stamina_actor>();
    JsonObject data = jsin.get_object();
    data.read( "progress", actor->activity_actor::progress );
    data.read( "stamina_threshold", actor->stamina_threshold );
    data.read( "stamina_initial", actor->stamina_initial );
    return actor;
}

std::unique_ptr<activity_actor> wait_stamina_actor::legacy_deserialize( const JsonObject &data )
{
    auto actor = std::make_unique<wait_stamina_actor>();

    auto values = data.get_int_array( "values" );
    if( !values.empty() ) {
        actor->stamina_threshold = values[0];
    }
    if( values.size() >= 2 ) {
        actor->stamina_initial = values[1];
    }

    return actor;
}

void wait_stamina_actor::start( player_activity &act, Character &who ) {}
void wait_stamina_actor::do_turn( player_activity &act, Character &who ) {}
void wait_stamina_actor::finish( player_activity &act, Character &who ) {}

// ─────────────────────────────────────────────────────────────────────────────
// hand_crank_charge_actor (ACT_HAND_CRANK)
// ─────────────────────────────────────────────────────────────────────────────

hand_crank_charge_actor::hand_crank_charge_actor(
    int interval_turns,
    int charges,
    int fatigue,
    const itype_id &ammo
) : charge_interval_turns( interval_turns )
  , charge_amount( charges )
  , fatigue_amount( fatigue )
  , ammo_type( ammo )
{}

void hand_crank_charge_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();
    jsout.member( "progress", activity_actor::progress );
    jsout.member( "charge_interval_turns", charge_interval_turns );
    jsout.member( "charge_amount", charge_amount );
    jsout.member( "fatigue_amount", fatigue_amount );
    jsout.member( "ammo_type", ammo_type );
    jsout.end_object();
}

std::unique_ptr<activity_actor> hand_crank_charge_actor::deserialize( JsonIn &jsin )
{
    auto actor = std::make_unique<hand_crank_charge_actor>();
    JsonObject data = jsin.get_object();
    data.read( "progress", actor->activity_actor::progress );
    data.read( "charge_interval_turns", actor->charge_interval_turns );
    data.read( "charge_amount", actor->charge_amount );
    data.read( "fatigue_amount", actor->fatigue_amount );
    data.read( "ammo_type", actor->ammo_type );
    return actor;
}

std::unique_ptr<activity_actor> hand_crank_charge_actor::legacy_deserialize( const JsonObject &data )
{
    auto actor = std::make_unique<hand_crank_charge_actor>();

    auto values = data.get_int_array( "values" );
    if( values.size() >= 1 ) {
        actor->charge_interval_turns = values[0];
    }
    if( values.size() >= 2 ) {
        actor->charge_amount = std::max( 1, values[1] );
    }
    if( values.size() >= 3 ) {
        actor->fatigue_amount = std::max( 0, values[2] );
    }

    auto str_values = data.get_string_array( "str_values" );
    if( !str_values.empty() && !str_values[0].empty() ) {
        actor->ammo_type = itype_id( str_values[0] );
    }

    return actor;
}

void hand_crank_charge_actor::start( player_activity &act, Character &who ) {}
void hand_crank_charge_actor::do_turn( player_activity &act, Character &who ) {}
void hand_crank_charge_actor::finish( player_activity &act, Character &who ) {}

// ─────────────────────────────────────────────────────────────────────────────
// wait_npc_actor (ACT_WAIT_NPC)
// ─────────────────────────────────────────────────────────────────────────────

wait_npc_actor::wait_npc_actor( const std::string &name )
    : npc_name( name )
{}

void wait_npc_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();
    jsout.member( "progress", activity_actor::progress );
    jsout.member( "npc_name", npc_name );
    jsout.end_object();
}

std::unique_ptr<activity_actor> wait_npc_actor::deserialize( JsonIn &jsin )
{
    auto actor = std::make_unique<wait_npc_actor>();
    JsonObject data = jsin.get_object();
    data.read( "progress", actor->activity_actor::progress );
    data.read( "npc_name", actor->npc_name );
    return actor;
}

std::unique_ptr<activity_actor> wait_npc_actor::legacy_deserialize( const JsonObject &data )
{
    auto str_values = data.get_string_array( "str_values" );
    if( str_values.empty() ) {
        return nullptr;
    }
    return std::make_unique<wait_npc_actor>( str_values[0] );
}

void wait_npc_actor::start( player_activity &act, Character &who ) {}
void wait_npc_actor::do_turn( player_activity &act, Character &who ) {}
void wait_npc_actor::finish( player_activity &act, Character &who ) {}

// ─────────────────────────────────────────────────────────────────────────────
// clear_rubble_actor (ACT_CLEAR_RUBBLE)
// ─────────────────────────────────────────────────────────────────────────────

clear_rubble_actor::clear_rubble_actor( const tripoint_abs_ms &pos )
    : rubble_pos( pos )
{}

void clear_rubble_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();
    jsout.member( "progress", activity_actor::progress );
    jsout.member( "rubble_pos", rubble_pos );
    jsout.end_object();
}

std::unique_ptr<activity_actor> clear_rubble_actor::deserialize( JsonIn &jsin )
{
    auto actor = std::make_unique<clear_rubble_actor>();
    JsonObject data = jsin.get_object();
    data.read( "progress", actor->activity_actor::progress );
    data.read( "rubble_pos", actor->rubble_pos );
    return actor;
}

std::unique_ptr<activity_actor> clear_rubble_actor::legacy_deserialize( const JsonObject &data )
{
    auto coords = std::vector<tripoint_abs_ms>();
    data.read( "coords", coords );
    if( coords.empty() ) {
        return nullptr;
    }
    return std::make_unique<clear_rubble_actor>( coords[0] );
}

void clear_rubble_actor::start( player_activity &act, Character &who ) {}
void clear_rubble_actor::do_turn( player_activity &act, Character &who ) {}
void clear_rubble_actor::finish( player_activity &act, Character &who ) {}

// ─────────────────────────────────────────────────────────────────────────────
// read_activity_actor (ACT_READ)
// ─────────────────────────────────────────────────────────────────────────────

read_activity_actor::read_activity_actor(
    safe_reference<item> book_ref,
    std::vector<npc_learner> npcs,
    bool martial_arts
) : book( std::move( book_ref ) )
  , learners( std::move( npcs ) )
  , is_martial_arts( martial_arts )
{}

void read_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();
    jsout.member( "progress", activity_actor::progress );
    jsout.member( "book", book );
    jsout.member( "is_martial_arts", is_martial_arts );
    jsout.member( "learners" );
    jsout.start_array();
    for( const auto &l : learners ) {
        jsout.start_object();
        jsout.member( "id", l.id );
        jsout.member( "penalty", l.penalty );
        jsout.end_object();
    }
    jsout.end_array();
    jsout.end_object();
}

std::unique_ptr<activity_actor> read_activity_actor::deserialize( JsonIn &jsin )
{
    auto actor = std::make_unique<read_activity_actor>();
    JsonObject data = jsin.get_object();
    data.read( "progress", actor->activity_actor::progress );
    data.read( "book", actor->book );
    data.read( "is_martial_arts", actor->is_martial_arts );

    JsonArray arr = data.get_array( "learners" );
    for( JsonObject lobj : arr ) {
        npc_learner l;
        lobj.read( "id", l.id );
        lobj.read( "penalty", l.penalty );
        actor->learners.push_back( l );
    }

    return actor;
}

std::unique_ptr<activity_actor> read_activity_actor::legacy_deserialize( const JsonObject &data )
{
    auto actor = std::make_unique<read_activity_actor>();

    // Check for martial arts flag first
    auto str_values = data.get_string_array( "str_values" );
    if( str_values.size() == 1 && str_values[0] == "martial_art" ) {
        actor->is_martial_arts = true;
    } else {
        // Read NPC learners from values[] and str_values[]
        auto values = data.get_int_array( "values" );
        size_t count = std::min( values.size(), str_values.size() );
        for( size_t i = 0; i < count; i++ ) {
            npc_learner l;
            l.id = character_id( values[i] );
            l.penalty = std::stof( str_values[i] );
            actor->learners.push_back( l );
        }
    }

    // Book from targets[0]
    auto targets = std::vector<safe_reference<item>>();
    data.read( "targets", targets );
    if( !targets.empty() ) {
        actor->book = std::move( targets[0] );
    }

    return actor;
}

void read_activity_actor::start( player_activity &act, Character &who ) {}
void read_activity_actor::do_turn( player_activity &act, Character &who ) {}
void read_activity_actor::finish( player_activity &act, Character &who ) {}

// ─────────────────────────────────────────────────────────────────────────────
// move_loot_activity_actor (ACT_MOVE_LOOT)
// ─────────────────────────────────────────────────────────────────────────────

move_loot_activity_actor::move_loot_activity_actor(
    int processed,
    int init_stage,
    const std::unordered_set<tripoint_abs_ms> &zpoints
) : items_processed( processed )
  , stage( init_stage )
  , zone_points( zpoints )
{}

void move_loot_activity_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();
    jsout.member( "progress", activity_actor::progress );
    jsout.member( "items_processed", items_processed );
    jsout.member( "stage", stage );
    jsout.member( "zone_points", zone_points );
    jsout.end_object();
}

std::unique_ptr<activity_actor> move_loot_activity_actor::deserialize( JsonIn &jsin )
{
    auto actor = std::make_unique<move_loot_activity_actor>();
    JsonObject data = jsin.get_object();
    data.read( "progress", actor->activity_actor::progress );
    data.read( "items_processed", actor->items_processed );
    data.read( "stage", actor->stage );
    data.read( "zone_points", actor->zone_points );
    return actor;
}

std::unique_ptr<activity_actor> move_loot_activity_actor::legacy_deserialize( const JsonObject &data )
{
    auto actor = std::make_unique<move_loot_activity_actor>();

    auto values = data.get_int_array( "values" );
    if( !values.empty() ) {
        actor->items_processed = values[0];
    }
    actor->stage = data.get_int( "index", 0 );

    data.read( "coord_set", actor->zone_points );

    return actor;
}

void move_loot_activity_actor::start( player_activity &act, Character &who ) {}
void move_loot_activity_actor::do_turn( player_activity &act, Character &who ) {}
void move_loot_activity_actor::finish( player_activity &act, Character &who ) {}

// ─────────────────────────────────────────────────────────────────────────────
// fetch_required_actor (ACT_FETCH_REQUIRED)
// ─────────────────────────────────────────────────────────────────────────────

fetch_required_actor::fetch_required_actor(
    do_activity_reason reason,
    const requirement_data &reqs,
    const tripoint_abs_ms &placement,
    const tripoint_abs_ms &source_zone
) : reason( reason )
  , fetch_requirements( reqs )
  , placement_pos( placement )
  , source_zone_pos( source_zone )
{}

void fetch_required_actor::serialize( JsonOut &jsout ) const
{
    jsout.start_object();
    jsout.member( "progress", activity_actor::progress );
    jsout.member( "reason", static_cast<int>( reason ) );
    jsout.member( "fetch_requirements", fetch_requirements );
    jsout.member( "placement_pos", placement_pos );
    jsout.member( "source_zone_pos", source_zone_pos );
    jsout.end_object();
}

std::unique_ptr<activity_actor> fetch_required_actor::deserialize( JsonIn &jsin )
{
    auto actor = std::make_unique<fetch_required_actor>();
    JsonObject data = jsin.get_object();
    data.read( "progress", actor->activity_actor::progress );

    int r = 0;
    data.read( "reason", r );
    actor->reason = static_cast<do_activity_reason>( r );

    data.read( "fetch_requirements", actor->fetch_requirements );
    data.read( "placement_pos", actor->placement_pos );
    data.read( "source_zone_pos", actor->source_zone_pos );
    return actor;
}

std::unique_ptr<activity_actor> fetch_required_actor::legacy_deserialize( const JsonObject &data )
{
    auto actor = std::make_unique<fetch_required_actor>();

    auto values = data.get_int_array( "values" );
    if( !values.empty() ) {
        actor->reason = static_cast<do_activity_reason>( values[0] );
    }

    // requirement string from str_values[0]
    auto str_values = data.get_string_array( "str_values" );
    if( !str_values.empty() ) {
        requirement_id req_id( str_values[0] );
        if( req_id.is_valid() ) {
            actor->fetch_requirements = req_id.obj();
        }
    }

    // placement_pos from coords[0]
    auto coords = std::vector<tripoint_abs_ms>();
    data.read( "coords", coords );
    if( !coords.empty() ) {
        actor->placement_pos = coords[0];
    }

    // source_zone_pos from placement
    tripoint_abs_ms pl;
    data.read( "placement", pl );
    actor->source_zone_pos = pl;

    return actor;
}

void fetch_required_actor::start( player_activity &act, Character &who ) {}
void fetch_required_actor::do_turn( player_activity &act, Character &who ) {}
void fetch_required_actor::finish( player_activity &act, Character &who ) {}

// ─── tree_communion_actor ────────────────────────────────────────────────────

tree_communion_actor::tree_communion_actor( int turns ) : startup_turns( turns ) {}
void tree_communion_actor::serialize( JsonOut &jsout ) const {
    jsout.start_object(); jsout.member( "progress", activity_actor::progress );
    jsout.member( "startup_turns", startup_turns ); jsout.end_object();
}
std::unique_ptr<activity_actor> tree_communion_actor::deserialize( JsonIn &jsin ) {
    auto actor = std::make_unique<tree_communion_actor>();
    JsonObject data = jsin.get_object(); data.read( "progress", actor->activity_actor::progress );
    data.read( "startup_turns", actor->startup_turns ); return actor;
}
std::unique_ptr<activity_actor> tree_communion_actor::legacy_deserialize( const JsonObject &data ) {
    auto actor = std::make_unique<tree_communion_actor>();
    auto values = data.get_int_array( "values" );
    if( !values.empty() ) actor->startup_turns = values[0];
    return actor;
}
void tree_communion_actor::start( player_activity &, Character & ) {}
void tree_communion_actor::do_turn( player_activity &act, Character &who ) {
    act.id()->call_do_turn( &act, &static_cast<player &>( who ) );
}
void tree_communion_actor::finish( player_activity &act, Character &who ) {
    act.id()->call_finish( &act, &static_cast<player &>( who ) );
}

// ─── shear_actor ─────────────────────────────────────────────────────────────

shear_actor::shear_actor( const tripoint_abs_ms &pos ) : target_pos( pos ) {}
void shear_actor::serialize( JsonOut &jsout ) const {
    jsout.start_object(); jsout.member( "progress", activity_actor::progress );
    jsout.member( "target_pos", target_pos ); jsout.end_object();
}
std::unique_ptr<activity_actor> shear_actor::deserialize( JsonIn &jsin ) {
    auto actor = std::make_unique<shear_actor>();
    JsonObject data = jsin.get_object(); data.read( "progress", actor->activity_actor::progress );
    data.read( "target_pos", actor->target_pos ); return actor;
}
std::unique_ptr<activity_actor> shear_actor::legacy_deserialize( const JsonObject &data ) {
    auto coords = std::vector<tripoint_abs_ms>(); data.read( "coords", coords );
    if( coords.empty() ) return nullptr;
    return std::make_unique<shear_actor>( coords[0] );
}
void shear_actor::start( player_activity &, Character & ) {}
void shear_actor::do_turn( player_activity &act, Character &who ) {
    act.id()->call_do_turn( &act, &static_cast<player &>( who ) );
}
void shear_actor::finish( player_activity &act, Character &who ) {
    act.id()->call_finish( &act, &static_cast<player &>( who ) );
}

// ─── milk_actor ──────────────────────────────────────────────────────────────

milk_actor::milk_actor( const tripoint_abs_ms &pos ) : target_pos( pos ) {}
void milk_actor::serialize( JsonOut &jsout ) const {
    jsout.start_object(); jsout.member( "progress", activity_actor::progress );
    jsout.member( "target_pos", target_pos ); jsout.end_object();
}
std::unique_ptr<activity_actor> milk_actor::deserialize( JsonIn &jsin ) {
    auto actor = std::make_unique<milk_actor>();
    JsonObject data = jsin.get_object(); data.read( "progress", actor->activity_actor::progress );
    data.read( "target_pos", actor->target_pos ); return actor;
}
std::unique_ptr<activity_actor> milk_actor::legacy_deserialize( const JsonObject &data ) {
    auto coords = std::vector<tripoint_abs_ms>(); data.read( "coords", coords );
    if( coords.empty() ) return nullptr;
    return std::make_unique<milk_actor>( coords[0] );
}
void milk_actor::start( player_activity &, Character & ) {}
void milk_actor::do_turn( player_activity &act, Character &who ) {
    act.id()->call_do_turn( &act, &static_cast<player &>( who ) );
}
void milk_actor::finish( player_activity &act, Character &who ) {
    act.id()->call_finish( &act, &static_cast<player &>( who ) );
}

// ─── pulp_actor ──────────────────────────────────────────────────────────────

pulp_actor::pulp_actor( const tripoint_abs_ms &pos, bool auto_no_acid )
    : target_pos( pos ), auto_pulp_no_acid( auto_no_acid ) {}
void pulp_actor::serialize( JsonOut &jsout ) const {
    jsout.start_object(); jsout.member( "progress", activity_actor::progress );
    jsout.member( "target_pos", target_pos );
    jsout.member( "auto_pulp_no_acid", auto_pulp_no_acid ); jsout.end_object();
}
std::unique_ptr<activity_actor> pulp_actor::deserialize( JsonIn &jsin ) {
    auto actor = std::make_unique<pulp_actor>();
    JsonObject data = jsin.get_object(); data.read( "progress", actor->activity_actor::progress );
    data.read( "target_pos", actor->target_pos );
    data.read( "auto_pulp_no_acid", actor->auto_pulp_no_acid ); return actor;
}
std::unique_ptr<activity_actor> pulp_actor::legacy_deserialize( const JsonObject &data ) {
    auto actor = std::make_unique<pulp_actor>();
    tripoint_abs_ms pl; data.read( "placement", pl );
    actor->target_pos = pl;
    auto str_values = data.get_string_array( "str_values" );
    if( !str_values.empty() && str_values[0] == "auto_pulp_no_acid" ) {
        actor->auto_pulp_no_acid = true;
    }
    return actor;
}
void pulp_actor::start( player_activity &, Character & ) {}
void pulp_actor::do_turn( player_activity &act, Character &who ) {
    act.id()->call_do_turn( &act, &static_cast<player &>( who ) );
}
void pulp_actor::finish( player_activity &act, Character &who ) {
    act.id()->call_finish( &act, &static_cast<player &>( who ) );
}

// ─── hotwire_car_actor ───────────────────────────────────────────────────────

hotwire_car_actor::hotwire_car_actor( const tripoint_abs_ms &pos, int skill )
    : veh_pos( pos ), mechanics_skill( skill ) {}
void hotwire_car_actor::serialize( JsonOut &jsout ) const {
    jsout.start_object(); jsout.member( "progress", activity_actor::progress );
    jsout.member( "veh_pos", veh_pos ); jsout.member( "mechanics_skill", mechanics_skill );
    jsout.end_object();
}
std::unique_ptr<activity_actor> hotwire_car_actor::deserialize( JsonIn &jsin ) {
    auto actor = std::make_unique<hotwire_car_actor>();
    JsonObject data = jsin.get_object(); data.read( "progress", actor->activity_actor::progress );
    data.read( "veh_pos", actor->veh_pos ); data.read( "mechanics_skill", actor->mechanics_skill );
    return actor;
}
std::unique_ptr<activity_actor> hotwire_car_actor::legacy_deserialize( const JsonObject &data ) {
    auto values = data.get_int_array( "values" );
    if( values.size() < 3 ) return nullptr;
    return std::make_unique<hotwire_car_actor>(
        tripoint_abs_ms( values[0], values[1], 0 ), values[2] );
}
void hotwire_car_actor::start( player_activity &, Character & ) {}
void hotwire_car_actor::do_turn( player_activity &act, Character &who ) {
    act.id()->call_do_turn( &act, &static_cast<player &>( who ) );
}
void hotwire_car_actor::finish( player_activity &act, Character &who ) {
    act.id()->call_finish( &act, &static_cast<player &>( who ) );
}

// ─── start_engines_actor ─────────────────────────────────────────────────────

start_engines_actor::start_engines_actor( int control, const tripoint_abs_ms &pos )
    : take_control( control ), placement( pos ) {}
void start_engines_actor::serialize( JsonOut &jsout ) const {
    jsout.start_object(); jsout.member( "progress", activity_actor::progress );
    jsout.member( "take_control", take_control ); jsout.member( "placement", placement );
    jsout.end_object();
}
std::unique_ptr<activity_actor> start_engines_actor::deserialize( JsonIn &jsin ) {
    auto actor = std::make_unique<start_engines_actor>();
    JsonObject data = jsin.get_object(); data.read( "progress", actor->activity_actor::progress );
    data.read( "take_control", actor->take_control ); data.read( "placement", actor->placement );
    return actor;
}
std::unique_ptr<activity_actor> start_engines_actor::legacy_deserialize( const JsonObject &data ) {
    auto actor = std::make_unique<start_engines_actor>();
    auto values = data.get_int_array( "values" );
    if( !values.empty() ) actor->take_control = values[0];
    data.read( "placement", actor->placement );
    return actor;
}
void start_engines_actor::start( player_activity &, Character & ) {}
void start_engines_actor::do_turn( player_activity &act, Character &who ) {
    act.id()->call_do_turn( &act, &static_cast<player &>( who ) );
}
void start_engines_actor::finish( player_activity &act, Character &who ) {
    act.id()->call_finish( &act, &static_cast<player &>( who ) );
}

// ─── start_fire_actor ────────────────────────────────────────────────────────

start_fire_actor::start_fire_actor( int light, const tripoint_abs_ms &pos )
    : light_level( light ), placement( pos ) {}
void start_fire_actor::serialize( JsonOut &jsout ) const {
    jsout.start_object(); jsout.member( "progress", activity_actor::progress );
    jsout.member( "light_level", light_level ); jsout.member( "placement", placement );
    jsout.end_object();
}
std::unique_ptr<activity_actor> start_fire_actor::deserialize( JsonIn &jsin ) {
    auto actor = std::make_unique<start_fire_actor>();
    JsonObject data = jsin.get_object(); data.read( "progress", actor->activity_actor::progress );
    data.read( "light_level", actor->light_level ); data.read( "placement", actor->placement );
    return actor;
}
std::unique_ptr<activity_actor> start_fire_actor::legacy_deserialize( const JsonObject &data ) {
    auto actor = std::make_unique<start_fire_actor>();
    auto values = data.get_int_array( "values" );
    if( !values.empty() ) actor->light_level = values[0];
    data.read( "placement", actor->placement );
    return actor;
}
void start_fire_actor::start( player_activity &, Character & ) {}
void start_fire_actor::do_turn( player_activity &act, Character &who ) {
    act.id()->call_do_turn( &act, &static_cast<player &>( who ) );
}
void start_fire_actor::finish( player_activity &act, Character &who ) {
    act.id()->call_finish( &act, &static_cast<player &>( who ) );
}

// ─── make_zlave_actor ────────────────────────────────────────────────────────

make_zlave_actor::make_zlave_actor( int success, const std::string &name )
    : success_chance( success ), corpse_name( name ) {}
void make_zlave_actor::serialize( JsonOut &jsout ) const {
    jsout.start_object(); jsout.member( "progress", activity_actor::progress );
    jsout.member( "success_chance", success_chance ); jsout.member( "corpse_name", corpse_name );
    jsout.end_object();
}
std::unique_ptr<activity_actor> make_zlave_actor::deserialize( JsonIn &jsin ) {
    auto actor = std::make_unique<make_zlave_actor>();
    JsonObject data = jsin.get_object(); data.read( "progress", actor->activity_actor::progress );
    data.read( "success_chance", actor->success_chance ); data.read( "corpse_name", actor->corpse_name );
    return actor;
}
std::unique_ptr<activity_actor> make_zlave_actor::legacy_deserialize( const JsonObject &data ) {
    auto actor = std::make_unique<make_zlave_actor>();
    auto values = data.get_int_array( "values" );
    if( !values.empty() ) actor->success_chance = values[0];
    auto str_values = data.get_string_array( "str_values" );
    if( !str_values.empty() ) actor->corpse_name = str_values[0];
    return actor;
}
void make_zlave_actor::start( player_activity &, Character & ) {}
void make_zlave_actor::do_turn( player_activity &act, Character &who ) {
    act.id()->call_do_turn( &act, &static_cast<player &>( who ) );
}
void make_zlave_actor::finish( player_activity &act, Character &who ) {
    act.id()->call_finish( &act, &static_cast<player &>( who ) );
}

// ─── study_spell_actor ───────────────────────────────────────────────────────

study_spell_actor::study_spell_actor( const std::string &type ) : spell_type( type ) {}
void study_spell_actor::serialize( JsonOut &jsout ) const {
    jsout.start_object(); jsout.member( "progress", activity_actor::progress );
    jsout.member( "spell_type", spell_type ); jsout.end_object();
}
std::unique_ptr<activity_actor> study_spell_actor::deserialize( JsonIn &jsin ) {
    auto actor = std::make_unique<study_spell_actor>();
    JsonObject data = jsin.get_object(); data.read( "progress", actor->activity_actor::progress );
    data.read( "spell_type", actor->spell_type ); return actor;
}
std::unique_ptr<activity_actor> study_spell_actor::legacy_deserialize( const JsonObject &data ) {
    auto str_values = data.get_string_array( "str_values" );
    if( str_values.empty() ) return nullptr;
    return std::make_unique<study_spell_actor>( str_values[0] );
}
void study_spell_actor::start( player_activity &, Character & ) {}
void study_spell_actor::do_turn( player_activity &act, Character &who ) {
    act.id()->call_do_turn( &act, &static_cast<player &>( who ) );
}
void study_spell_actor::finish( player_activity &act, Character &who ) {
    act.id()->call_finish( &act, &static_cast<player &>( who ) );
}

// ─── firstaid_actor ──────────────────────────────────────────────────────────

firstaid_actor::firstaid_actor( const std::string &type ) : heal_type( type ) {}
void firstaid_actor::serialize( JsonOut &jsout ) const {
    jsout.start_object(); jsout.member( "progress", activity_actor::progress );
    jsout.member( "heal_type", heal_type ); jsout.end_object();
}
std::unique_ptr<activity_actor> firstaid_actor::deserialize( JsonIn &jsin ) {
    auto actor = std::make_unique<firstaid_actor>();
    JsonObject data = jsin.get_object(); data.read( "progress", actor->activity_actor::progress );
    data.read( "heal_type", actor->heal_type ); return actor;
}
std::unique_ptr<activity_actor> firstaid_actor::legacy_deserialize( const JsonObject &data ) {
    auto str_values = data.get_string_array( "str_values" );
    if( str_values.empty() ) return nullptr;
    return std::make_unique<firstaid_actor>( str_values[0] );
}
void firstaid_actor::start( player_activity &, Character & ) {}
void firstaid_actor::do_turn( player_activity &act, Character &who ) {
    act.id()->call_do_turn( &act, &static_cast<player &>( who ) );
}
void firstaid_actor::finish( player_activity &act, Character &who ) {
    act.id()->call_finish( &act, &static_cast<player &>( who ) );
}

// ─── play_with_pet_actor ─────────────────────────────────────────────────────

play_with_pet_actor::play_with_pet_actor( weak_ptr_fast<monster> pet_ref, const std::string &name )
    : pet( std::move( pet_ref ) ), pet_name( name ) {}
void play_with_pet_actor::serialize( JsonOut &jsout ) const {
    jsout.start_object(); jsout.member( "progress", activity_actor::progress );
    jsout.member( "pet_name", pet_name ); jsout.end_object();
}
std::unique_ptr<activity_actor> play_with_pet_actor::deserialize( JsonIn &jsin ) {
    auto actor = std::make_unique<play_with_pet_actor>();
    JsonObject data = jsin.get_object(); data.read( "progress", actor->activity_actor::progress );
    data.read( "pet_name", actor->pet_name ); return actor;
}
std::unique_ptr<activity_actor> play_with_pet_actor::legacy_deserialize( const JsonObject &data ) {
    auto actor = std::make_unique<play_with_pet_actor>();
    auto str_values = data.get_string_array( "str_values" );
    if( !str_values.empty() ) actor->pet_name = str_values[0];
    // monster weak_ptr is runtime-only; re-acquired at activity start
    return actor;
}
void play_with_pet_actor::start( player_activity &, Character & ) {}
void play_with_pet_actor::do_turn( player_activity &act, Character &who ) {
    act.id()->call_do_turn( &act, &static_cast<player &>( who ) );
}
void play_with_pet_actor::finish( player_activity &act, Character &who ) {
    act.id()->call_finish( &act, &static_cast<player &>( who ) );
}

// ─── train_pet_actor ─────────────────────────────────────────────────────────

train_pet_actor::train_pet_actor( weak_ptr_fast<monster> pet_ref, const std::string &name )
    : pet( std::move( pet_ref ) ), pet_name( name ) {}
void train_pet_actor::serialize( JsonOut &jsout ) const {
    jsout.start_object(); jsout.member( "progress", activity_actor::progress );
    jsout.member( "pet_name", pet_name ); jsout.end_object();
}
std::unique_ptr<activity_actor> train_pet_actor::deserialize( JsonIn &jsin ) {
    auto actor = std::make_unique<train_pet_actor>();
    JsonObject data = jsin.get_object(); data.read( "progress", actor->activity_actor::progress );
    data.read( "pet_name", actor->pet_name ); return actor;
}
std::unique_ptr<activity_actor> train_pet_actor::legacy_deserialize( const JsonObject &data ) {
    auto actor = std::make_unique<train_pet_actor>();
    auto str_values = data.get_string_array( "str_values" );
    if( !str_values.empty() ) actor->pet_name = str_values[0];
    // monster weak_ptr is runtime-only; re-acquired at activity start
    return actor;
}
void train_pet_actor::start( player_activity &, Character & ) {}
void train_pet_actor::do_turn( player_activity &act, Character &who ) {
    act.id()->call_do_turn( &act, &static_cast<player &>( who ) );
}
void train_pet_actor::finish( player_activity &act, Character &who ) {
    act.id()->call_finish( &act, &static_cast<player &>( who ) );
}

// ─── socialize_actor ─────────────────────────────────────────────────────────

socialize_actor::socialize_actor( const std::string &name ) : npc_name( name ) {}
void socialize_actor::serialize( JsonOut &jsout ) const {
    jsout.start_object(); jsout.member( "progress", activity_actor::progress );
    jsout.member( "npc_name", npc_name ); jsout.end_object();
}
std::unique_ptr<activity_actor> socialize_actor::deserialize( JsonIn &jsin ) {
    auto actor = std::make_unique<socialize_actor>();
    JsonObject data = jsin.get_object(); data.read( "progress", actor->activity_actor::progress );
    data.read( "npc_name", actor->npc_name ); return actor;
}
std::unique_ptr<activity_actor> socialize_actor::legacy_deserialize( const JsonObject &data ) {
    auto str_values = data.get_string_array( "str_values" );
    if( str_values.empty() ) return nullptr;
    return std::make_unique<socialize_actor>( str_values[0] );
}
void socialize_actor::start( player_activity &, Character & ) {}
void socialize_actor::do_turn( player_activity &act, Character &who ) {
    act.id()->call_do_turn( &act, &static_cast<player &>( who ) );
}
void socialize_actor::finish( player_activity &act, Character &who ) {
    act.id()->call_finish( &act, &static_cast<player &>( who ) );
}

// ─── train_actor ─────────────────────────────────────────────────────────────

train_actor::train_actor( const std::string &name ) : skill_name( name ) {}
void train_actor::serialize( JsonOut &jsout ) const {
    jsout.start_object(); jsout.member( "progress", activity_actor::progress );
    jsout.member( "skill_name", skill_name ); jsout.end_object();
}
std::unique_ptr<activity_actor> train_actor::deserialize( JsonIn &jsin ) {
    auto actor = std::make_unique<train_actor>();
    JsonObject data = jsin.get_object(); data.read( "progress", actor->activity_actor::progress );
    data.read( "skill_name", actor->skill_name ); return actor;
}
std::unique_ptr<activity_actor> train_actor::legacy_deserialize( const JsonObject &data ) {
    return std::make_unique<train_actor>( data.get_string( "name" ) );
}
void train_actor::start( player_activity &, Character & ) {}
void train_actor::do_turn( player_activity &act, Character &who ) {
    act.id()->call_do_turn( &act, &static_cast<player &>( who ) );
}
void train_actor::finish( player_activity &act, Character &who ) {
    act.id()->call_finish( &act, &static_cast<player &>( who ) );
}

// ─── butcher_actor ───────────────────────────────────────────────────────────

butcher_actor::butcher_actor( const activity_id &type, safe_reference<item> corpse_ref )
    : act_type( type ), corpse( std::move( corpse_ref ) ) {}
void butcher_actor::serialize( JsonOut &jsout ) const {
    jsout.start_object(); jsout.member( "progress", activity_actor::progress );
    jsout.member( "act_type", act_type ); jsout.member( "corpse", corpse ); jsout.end_object();
}
std::unique_ptr<activity_actor> butcher_actor::deserialize( JsonIn &jsin ) {
    auto actor = std::make_unique<butcher_actor>();
    JsonObject data = jsin.get_object(); data.read( "progress", actor->activity_actor::progress );
    data.read( "act_type", actor->act_type ); data.read( "corpse", actor->corpse ); return actor;
}
std::unique_ptr<activity_actor> butcher_actor::legacy_deserialize( const JsonObject &data ) {
    // Read activity type from the save data itself
    activity_id act_type( data.get_string( "type" ) );
    auto targets = std::vector<safe_reference<item>>();
    data.read( "targets", targets );
    if( targets.empty() ) return nullptr;
    return std::make_unique<butcher_actor>( act_type, std::move( targets[0] ) );
}
void butcher_actor::start( player_activity &, Character & ) {}
void butcher_actor::do_turn( player_activity &act, Character &who ) {
    act.id()->call_do_turn( &act, &static_cast<player &>( who ) );
}
void butcher_actor::finish( player_activity &act, Character &who ) {
    act.id()->call_finish( &act, &static_cast<player &>( who ) );
}

// ─── operation_actor ─────────────────────────────────────────────────────────

operation_actor::operation_actor( const std::string &type, const std::string &bid,
    const std::string &installer, bool adoc, int diff, int succ, int cap, int skill )
    : op_type( type ), bionic_id( bid ), installer_name( installer ),
    autodoc( adoc ), difficulty( diff ), success( succ ), capacity( cap ), pl_skill( skill ) {}
void operation_actor::serialize( JsonOut &jsout ) const {
    jsout.start_object(); jsout.member( "progress", activity_actor::progress );
    jsout.member( "op_type", op_type ); jsout.member( "bionic_id", bionic_id );
    jsout.member( "installer_name", installer_name ); jsout.member( "autodoc", autodoc );
    jsout.member( "difficulty", difficulty ); jsout.member( "success", success );
    jsout.member( "capacity", capacity ); jsout.member( "pl_skill", pl_skill );
    jsout.end_object();
}
std::unique_ptr<activity_actor> operation_actor::deserialize( JsonIn &jsin ) {
    auto actor = std::make_unique<operation_actor>();
    JsonObject data = jsin.get_object(); data.read( "progress", actor->activity_actor::progress );
    data.read( "op_type", actor->op_type ); data.read( "bionic_id", actor->bionic_id );
    data.read( "installer_name", actor->installer_name ); data.read( "autodoc", actor->autodoc );
    data.read( "difficulty", actor->difficulty ); data.read( "success", actor->success );
    data.read( "capacity", actor->capacity ); data.read( "pl_skill", actor->pl_skill );
    return actor;
}
std::unique_ptr<activity_actor> operation_actor::legacy_deserialize( const JsonObject &data ) {
    auto actor = std::make_unique<operation_actor>();
    auto values = data.get_int_array( "values" );
    if( values.size() >= 4 ) {
        actor->difficulty = values[0]; actor->success = values[1];
        actor->capacity = values[2]; actor->pl_skill = values[3];
    }
    auto str_values = data.get_string_array( "str_values" );
    if( str_values.size() >= 1 ) actor->op_type = str_values[0];
    if( str_values.size() >= 2 ) actor->bionic_id = str_values[1];
    if( str_values.size() >= 3 ) actor->installer_name = str_values[2];
    if( str_values.size() >= 4 ) actor->autodoc = ( str_values[3] == "true" );
    return actor;
}
void operation_actor::start( player_activity &, Character & ) {}
void operation_actor::do_turn( player_activity &act, Character &who ) {
    act.id()->call_do_turn( &act, &static_cast<player &>( who ) );
}
void operation_actor::finish( player_activity &act, Character &who ) {
    act.id()->call_finish( &act, &static_cast<player &>( who ) );
}

// ─── gunmod_add_actor ────────────────────────────────────────────────────────

gunmod_add_actor::gunmod_add_actor( int r, int rk, int q,
    const std::string &tool, safe_reference<item> gun_ref, safe_reference<item> mod_ref )
    : roll( r ), risk( rk ), qty( q ), tool_name( tool ),
    gun( std::move( gun_ref ) ), mod( std::move( mod_ref ) ) {}
void gunmod_add_actor::serialize( JsonOut &jsout ) const {
    jsout.start_object(); jsout.member( "progress", activity_actor::progress );
    jsout.member( "roll", roll ); jsout.member( "risk", risk );
    jsout.member( "qty", qty ); jsout.member( "tool_name", tool_name );
    jsout.member( "gun", gun ); jsout.member( "mod", mod ); jsout.end_object();
}
std::unique_ptr<activity_actor> gunmod_add_actor::deserialize( JsonIn &jsin ) {
    auto actor = std::make_unique<gunmod_add_actor>();
    JsonObject data = jsin.get_object(); data.read( "progress", actor->activity_actor::progress );
    data.read( "roll", actor->roll ); data.read( "risk", actor->risk );
    data.read( "qty", actor->qty ); data.read( "tool_name", actor->tool_name );
    data.read( "gun", actor->gun ); data.read( "mod", actor->mod ); return actor;
}
std::unique_ptr<activity_actor> gunmod_add_actor::legacy_deserialize( const JsonObject &data ) {
    auto actor = std::make_unique<gunmod_add_actor>();
    auto values = data.get_int_array( "values" );
    if( values.size() >= 4 ) {
        actor->roll = values[1]; actor->risk = values[2]; actor->qty = values[3];
    }
    actor->tool_name = data.get_string( "name" );
    auto targets = std::vector<safe_reference<item>>();
    data.read( "targets", targets );
    if( targets.size() >= 1 ) actor->gun = std::move( targets[0] );
    if( targets.size() >= 2 ) actor->mod = std::move( targets[1] );
    return actor;
}
void gunmod_add_actor::start( player_activity &, Character & ) {}
void gunmod_add_actor::do_turn( player_activity &act, Character &who ) {
    act.id()->call_do_turn( &act, &static_cast<player &>( who ) );
}
void gunmod_add_actor::finish( player_activity &act, Character &who ) {
    act.id()->call_finish( &act, &static_cast<player &>( who ) );
}

// ─────────────────────────────────────────────────────────────────────────────
// Dispatch tables
// ─────────────────────────────────────────────────────────────────────────────

namespace activity_actors
{

// Please keep this alphabetically sorted
const std::unordered_map<activity_id, std::unique_ptr<activity_actor>( * )( JsonIn & )>
deserialize_functions = {
    { activity_id( "ACT_AIM" ), &aim_activity_actor::deserialize },
    { activity_id( "ACT_AUTODRIVE" ), &autodrive_activity_actor::deserialize },
    { activity_id( "ACT_BOLTCUTTING" ), &boltcutting_activity_actor::deserialize },
    { activity_id( "ACT_BUILD" ), &construction_activity_actor::deserialize },
    { activity_id( "ACT_CRAFT" ), &craft_activity_actor::deserialize },
    { activity_id( "ACT_DIG" ), &dig_activity_actor::deserialize },
    { activity_id( "ACT_FIRSTAID" ), &firstaid_actor::deserialize },
    { activity_id( "ACT_FETCH_REQUIRED" ), &fetch_required_actor::deserialize },
    { activity_id( "ACT_FIELD_DRESS" ), &butcher_actor::deserialize },
    { activity_id( "ACT_GUNMOD_ADD" ), &gunmod_add_actor::deserialize },
    { activity_id( "ACT_MOVE_LOOT" ), &move_loot_activity_actor::deserialize },
    { activity_id( "ACT_OPERATION" ), &operation_actor::deserialize },
    { activity_id( "ACT_DIG_CHANNEL" ), &dig_channel_activity_actor::deserialize },
    { activity_id( "ACT_DISASSEMBLE" ), &disassemble_activity_actor::deserialize },
    { activity_id( "ACT_DISMEMBER" ), &butcher_actor::deserialize },
    { activity_id( "ACT_DISSECT" ), &butcher_actor::deserialize },
    { activity_id( "ACT_DROP" ), &drop_activity_actor::deserialize },
    { activity_id( "ACT_HACKING" ), &hacking_activity_actor::deserialize },
    { activity_id( "ACT_HACKSAW" ), &hacksaw_activity_actor::deserialize },
    { activity_id( "ACT_LOCKPICK" ), &lockpick_activity_actor::deserialize },
    { activity_id( "ACT_MIGRATION_CANCEL" ), &migration_cancel_activity_actor::deserialize },
    { activity_id( "ACT_MOVE_ITEMS" ), &move_items_activity_actor::deserialize },
    { activity_id( "ACT_TOGGLE_GATE" ), &toggle_gate_activity_actor::deserialize },
    { activity_id( "ACT_OXYTORCH" ), &oxytorch_activity_actor::deserialize },
    { activity_id( "ACT_PICKUP" ), &pickup_activity_actor::deserialize },
    { activity_id( "ACT_READ" ), &read_activity_actor::deserialize },
    { activity_id( "ACT_SHEAR" ), &shear_actor::deserialize },
    { activity_id( "ACT_SOCIALIZE" ), &socialize_actor::deserialize },
    { activity_id( "ACT_START_ENGINES" ), &start_engines_actor::deserialize },
    { activity_id( "ACT_START_FIRE" ), &start_fire_actor::deserialize },
    { activity_id( "ACT_STASH" ), &stash_activity_actor::deserialize },
    { activity_id( "ACT_STUDY_SPELL" ), &study_spell_actor::deserialize },
    { activity_id( "ACT_THROW" ), &throw_activity_actor::deserialize },
    { activity_id( "ACT_TRAIN" ), &train_actor::deserialize },
    { activity_id( "ACT_ASSIST" ), &assist_activity_actor::deserialize },
    { activity_id( "ACT_BLEED" ), &butcher_actor::deserialize },
    { activity_id( "ACT_BUTCHER" ), &butcher_actor::deserialize },
    { activity_id( "ACT_BUTCHER_FULL" ), &butcher_actor::deserialize },
    { activity_id( "ACT_CLEAR_RUBBLE" ), &clear_rubble_actor::deserialize },
    { activity_id( "ACT_FILL_LIQUID" ), &liquid_transfer_actor::deserialize },
    { activity_id( "ACT_HAND_CRANK" ), &hand_crank_charge_actor::deserialize },
    { activity_id( "ACT_HOTWIRE_CAR" ), &hotwire_car_actor::deserialize },
    { activity_id( "ACT_LONGSALVAGE" ), &salvage_activity_actor::deserialize },
    { activity_id( "ACT_MAKE_ZLAVE" ), &make_zlave_actor::deserialize },
    { activity_id( "ACT_MILK" ), &milk_actor::deserialize },
    { activity_id( "ACT_PLAY_WITH_PET" ), &play_with_pet_actor::deserialize },
    { activity_id( "ACT_PULP" ), &pulp_actor::deserialize },
    { activity_id( "ACT_QUARTER" ), &butcher_actor::deserialize },
    { activity_id( "ACT_REPAIR_ITEM" ), &repair_actor::deserialize },
    { activity_id( "ACT_SKIN" ), &butcher_actor::deserialize },
    { activity_id( "ACT_TRAIN_PET" ), &train_pet_actor::deserialize },
    { activity_id( "ACT_TREE_COMMUNION" ), &tree_communion_actor::deserialize },
    { activity_id( "ACT_VEHICLE" ), &vehicle_work_actor::deserialize },
    { activity_id( "ACT_WAIT_NPC" ), &wait_npc_actor::deserialize },
    { activity_id( "ACT_WAIT_STAMINA" ), &wait_stamina_actor::deserialize },
    { activity_id( "ACT_WEAR" ), &wear_actor::deserialize }
};

const std::unordered_map<activity_id, std::unique_ptr<activity_actor>( * )( const JsonObject & )>
legacy_deserialize_functions = {
    { activity_id( "ACT_CLEAR_RUBBLE" ), &clear_rubble_actor::legacy_deserialize },
    { activity_id( "ACT_CRAFT" ), &craft_activity_actor::legacy_deserialize },
    { activity_id( "ACT_FETCH_REQUIRED" ), &fetch_required_actor::legacy_deserialize },
    { activity_id( "ACT_FIELD_DRESS" ), &butcher_actor::legacy_deserialize },
    { activity_id( "ACT_FILL_LIQUID" ), &liquid_transfer_actor::legacy_deserialize },
    { activity_id( "ACT_HAND_CRANK" ), &hand_crank_charge_actor::legacy_deserialize },
    { activity_id( "ACT_MOVE_LOOT" ), &move_loot_activity_actor::legacy_deserialize },
    { activity_id( "ACT_OPERATION" ), &operation_actor::legacy_deserialize },
    { activity_id( "ACT_READ" ), &read_activity_actor::legacy_deserialize },
    { activity_id( "ACT_REPAIR_ITEM" ), &repair_actor::legacy_deserialize },
    { activity_id( "ACT_VEHICLE" ), &vehicle_work_actor::legacy_deserialize },
    { activity_id( "ACT_WAIT_NPC" ), &wait_npc_actor::legacy_deserialize },
    { activity_id( "ACT_WAIT_STAMINA" ), &wait_stamina_actor::legacy_deserialize },
    { activity_id( "ACT_WEAR" ), &wear_actor::legacy_deserialize },
    { activity_id( "ACT_FIRSTAID" ), &firstaid_actor::legacy_deserialize },
    { activity_id( "ACT_GUNMOD_ADD" ), &gunmod_add_actor::legacy_deserialize },
    { activity_id( "ACT_HOTWIRE_CAR" ), &hotwire_car_actor::legacy_deserialize },
    { activity_id( "ACT_MAKE_ZLAVE" ), &make_zlave_actor::legacy_deserialize },
    { activity_id( "ACT_MILK" ), &milk_actor::legacy_deserialize },
    { activity_id( "ACT_PLAY_WITH_PET" ), &play_with_pet_actor::legacy_deserialize },
    { activity_id( "ACT_PULP" ), &pulp_actor::legacy_deserialize },
    { activity_id( "ACT_SHEAR" ), &shear_actor::legacy_deserialize },
    { activity_id( "ACT_SOCIALIZE" ), &socialize_actor::legacy_deserialize },
    { activity_id( "ACT_START_ENGINES" ), &start_engines_actor::legacy_deserialize },
    { activity_id( "ACT_START_FIRE" ), &start_fire_actor::legacy_deserialize },
    { activity_id( "ACT_STUDY_SPELL" ), &study_spell_actor::legacy_deserialize },
    { activity_id( "ACT_TRAIN" ), &train_actor::legacy_deserialize },
    { activity_id( "ACT_TRAIN_PET" ), &train_pet_actor::legacy_deserialize },
    { activity_id( "ACT_TREE_COMMUNION" ), &tree_communion_actor::legacy_deserialize },
    { activity_id( "ACT_BLEED" ), &butcher_actor::legacy_deserialize },
    { activity_id( "ACT_BUTCHER" ), &butcher_actor::legacy_deserialize },
    { activity_id( "ACT_BUTCHER_FULL" ), &butcher_actor::legacy_deserialize },
    { activity_id( "ACT_DISMEMBER" ), &butcher_actor::legacy_deserialize },
    { activity_id( "ACT_DISSECT" ), &butcher_actor::legacy_deserialize },
    { activity_id( "ACT_FIELD_DRESS" ), &butcher_actor::legacy_deserialize },
    { activity_id( "ACT_QUARTER" ), &butcher_actor::legacy_deserialize },
    { activity_id( "ACT_SKIN" ), &butcher_actor::legacy_deserialize }
};
} // namespace activity_actors

void serialize( const std::unique_ptr<activity_actor> &actor, JsonOut &jsout )
{
    if( !actor ) {
        jsout.write_null();
    } else {
        jsout.start_object();

        jsout.member( "actor_type", actor->get_type() );
        jsout.member( "actor_data", *actor );

        jsout.end_object();
    }
}

void deserialize( std::unique_ptr<activity_actor> &actor, JsonIn &jsin )
{
    if( jsin.test_null() ) {
        actor = nullptr;
    } else {
        JsonObject data = jsin.get_object();
        if( data.has_member( "actor_data" ) ) {
            activity_id actor_type;
            data.read( "actor_type", actor_type );
            auto deserializer = activity_actors::deserialize_functions.find( actor_type );
            if( deserializer != activity_actors::deserialize_functions.end() ) {
                actor = deserializer->second( *data.get_raw( "actor_data" ) );
            } else {
                debugmsg( "Failed to find activity actor deserializer for type \"%s\"", actor_type.c_str() );
                actor = nullptr;
            }
        } else {
            debugmsg( "Failed to load activity actor" );
            actor = nullptr;
        }
    }
}
