#include "eoc.h"

#include <string>
#include <vector>

#include "calendar.h"
#include "character.h"
#include "effect.h"
#include "json.h"
#include "rng.h"
#include "translations.h"
#include "units_serde.h"

// Effect-specific condition keys for dynamic messages
namespace effect_data
{
const std::unordered_set<std::string> effect_conds = { {
        "duration_greater", "duration_less",
        "intensity_equals", "intensity_greater", "intensity_less",
        "one_in"
    }
};
} // namespace effect_data

time_duration effect_context::get_duration() const
{
    if( eff ) {
        return eff->get_duration();
    }
    return 0_turns;
}

int effect_context::get_intensity() const
{
    if( eff ) {
        return eff->get_intensity();
    }
    return 0;
}

void effect_fun_t::set_mod_stored_kcal( int amount )
{
    function = [amount]( const effect_context & ctx ) {
        if( ctx.u ) {
            ctx.u->mod_stored_kcal( amount );
        }
    };
}

// Forward declarations for helper functions
static bool evaluate_effect_condition( const JsonObject &jo, const effect_context &ctx );
static void parse_effect_action( const JsonObject &jo, effect_fun_t &effect );

effect_message_t effect_message_t::from_member( const JsonObject &jo,
        const std::string &member_name )
{
    if( jo.has_array( member_name ) ) {
        return effect_message_t( jo.get_array( member_name ) );
    } else if( jo.has_object( member_name ) ) {
        return effect_message_t( jo.get_object( member_name ) );
    } else if( jo.has_string( member_name ) ) {
        return effect_message_t( jo.get_string( member_name ) );
    } else {
        return effect_message_t{};
    }
}

effect_message_t::effect_message_t( const std::string &msg )
{
    function = [msg]( const effect_context & ) {
        return _( msg );
    };
}

effect_message_t::effect_message_t( const JsonObject &jo )
{
    // Handle "and" - concatenate multiple messages
    if( jo.has_member( "and" ) ) {
        std::vector<effect_message_t> messages;
        for( const JsonValue entry : jo.get_array( "and" ) ) {
            if( entry.test_string() ) {
                messages.emplace_back( entry.get_string() );
            } else if( entry.test_array() ) {
                messages.emplace_back( entry.get_array() );
            } else if( entry.test_object() ) {
                messages.emplace_back( entry.get_object() );
            } else {
                entry.throw_error( "invalid format: must be string, array or object" );
            }
        }
        function = [messages]( const effect_context & ctx ) {
            std::string result;
            for( const effect_message_t &msg : messages ) {
                result += msg( ctx );
            }
            return result;
        };
        return;
    }

    // Handle "message" with optional "effect" - display message and run effects
    if( jo.has_member( "message" ) ) {
        const effect_message_t msg = from_member( jo, "message" );
        std::vector<effect_fun_t> effects;

        if( jo.has_array( "effect" ) ) {
            for( const JsonObject effect_jo : jo.get_array( "effect" ) ) {
                effect_fun_t eff;
                parse_effect_action( effect_jo, eff );
                effects.push_back( eff );
            }
        } else if( jo.has_object( "effect" ) ) {
            effect_fun_t eff;
            parse_effect_action( jo.get_object( "effect" ), eff );
            effects.push_back( eff );
        }

        function = [msg, effects]( const effect_context & ctx ) {
            std::string result = msg( ctx );
            for( const effect_fun_t &eff : effects ) {
                eff( ctx );
            }
            return result;
        };
        return;
    }

    // Handle conditional branching
    // Look for effect-specific conditions first
    bool found_condition = false;
    const effect_message_t yes = from_member( jo, "yes" );
    const effect_message_t no = from_member( jo, "no" );

    // Effect-specific conditions
    if( jo.has_member( "duration_greater" ) ) {
        found_condition = true;
        const time_duration threshold = read_from_json_string<time_duration>(
                                            *jo.get_raw( "duration_greater" ), time_duration::units );
        function = [threshold, yes, no]( const effect_context & ctx ) {
            return ( ctx.get_duration() > threshold ? yes : no )( ctx );
        };
    } else if( jo.has_member( "duration_less" ) ) {
        found_condition = true;
        const time_duration threshold = read_from_json_string<time_duration>(
                                            *jo.get_raw( "duration_less" ), time_duration::units );
        function = [threshold, yes, no]( const effect_context & ctx ) {
            return ( ctx.get_duration() < threshold ? yes : no )( ctx );
        };
    } else if( jo.has_int( "intensity_equals" ) ) {
        found_condition = true;
        const int val = jo.get_int( "intensity_equals" );
        function = [val, yes, no]( const effect_context & ctx ) {
            return ( ctx.get_intensity() == val ? yes : no )( ctx );
        };
    } else if( jo.has_int( "intensity_greater" ) ) {
        found_condition = true;
        const int val = jo.get_int( "intensity_greater" );
        function = [val, yes, no]( const effect_context & ctx ) {
            return ( ctx.get_intensity() > val ? yes : no )( ctx );
        };
    } else if( jo.has_int( "intensity_less" ) ) {
        found_condition = true;
        const int val = jo.get_int( "intensity_less" );
        function = [val, yes, no]( const effect_context & ctx ) {
            return ( ctx.get_intensity() < val ? yes : no )( ctx );
        };
    } else if( jo.has_int( "one_in" ) ) {
        found_condition = true;
        const int chance = jo.get_int( "one_in" );
        function = [chance, yes, no]( const effect_context & ctx ) {
            return ( one_in( chance ) ? yes : no )( ctx );
        };
    }

    // Character-based conditions (shared with dialogue system)
    // Supports both simple integer format and object format with comparison operator
    if( !found_condition && jo.has_member( "u_has_intelligence" ) ) {
        found_condition = true;
        int threshold = 0;
        std::string compare_op = ">=";  // default comparison

        if( jo.has_int( "u_has_intelligence" ) ) {
            threshold = jo.get_int( "u_has_intelligence" );
        } else if( jo.has_object( "u_has_intelligence" ) ) {
            JsonObject int_obj = jo.get_object( "u_has_intelligence" );
            threshold = int_obj.get_int( "value" );
            compare_op = int_obj.get_string( "compare", ">=" );
        }

        function = [threshold, compare_op, yes, no]( const effect_context & ctx ) {
            if( !ctx.u ) {
                return no( ctx );
            }
            bool result = false;
            int stat = ctx.u->int_cur;
            if( compare_op == ">" ) {
                result = stat > threshold;
            } else if( compare_op == ">=" ) {
                result = stat >= threshold;
            } else if( compare_op == "<" ) {
                result = stat < threshold;
            } else if( compare_op == "<=" ) {
                result = stat <= threshold;
            } else if( compare_op == "==" ) {
                result = stat == threshold;
            } else if( compare_op == "!=" ) {
                result = stat != threshold;
            }
            return ( result ? yes : no )( ctx );
        };
    } else if( !found_condition && jo.has_string( "u_has_trait" ) ) {
        found_condition = true;
        const std::string trait_str = jo.get_string( "u_has_trait" );
        const trait_id trait( trait_str );
        function = [trait, yes, no]( const effect_context & ctx ) {
            if( !ctx.u ) {
                return no( ctx );
            }
            return ( ctx.u->has_trait( trait ) ? yes : no )( ctx );
        };
    } else if( !found_condition && jo.has_string( "u_has_item" ) ) {
        found_condition = true;
        const std::string item_str = jo.get_string( "u_has_item" );
        const itype_id item_id( item_str );
        function = [item_id, yes, no]( const effect_context & ctx ) {
            if( !ctx.u ) {
                return no( ctx );
            }
            return ( ctx.u->has_amount( item_id, 1 ) ? yes : no )( ctx );
        };
    }

    if( !found_condition ) {
        jo.throw_error( "dynamic message condition not recognized" );
    }
}

effect_message_t::effect_message_t( const JsonArray &ja )
{
    std::vector<effect_message_t> messages;
    for( const JsonValue entry : ja ) {
        if( entry.test_string() ) {
            messages.emplace_back( entry.get_string() );
        } else if( entry.test_array() ) {
            messages.emplace_back( entry.get_array() );
        } else if( entry.test_object() ) {
            messages.emplace_back( entry.get_object() );
        } else {
            entry.throw_error( "invalid format: must be string, array or object" );
        }
    }
    // Random selection from array
    function = [messages]( const effect_context & ctx ) {
        const effect_message_t &msg = random_entry_ref( messages );
        return msg( ctx );
    };
}

static void parse_effect_action( const JsonObject &jo, effect_fun_t &effect )
{
    if( jo.has_int( "u_mod_stored_kcal" ) ) {
        int amount = jo.get_int( "u_mod_stored_kcal" );
        effect.set_mod_stored_kcal( amount );
    }
    // Can add more effect types here as needed
}

