#pragma once

#include <functional>
#include <string>
#include <vector>

#include "calendar.h"

class Character;
class effect;
class JsonArray;
class JsonObject;
struct dialogue;

/**
 * @brief Context for effect-on-condition evaluation.
 *
 * This provides a generic context that can be used for evaluating conditions
 * and generating dynamic messages for effects. It abstracts the common
 * pattern from dialogue to also work for effect messages.
 */
struct effect_context {
    /** The character this effect applies to. */
    Character *u = nullptr;
    /** The effect instance being processed. */
    const effect *eff = nullptr;

    effect_context() = default;
    effect_context( Character *who, const effect *effect_ptr )
        : u( who ), eff( effect_ptr ) {}

    /** Returns the current duration of the effect. */
    time_duration get_duration() const;
    /** Returns the current intensity of the effect. */
    int get_intensity() const;
};

/**
 * @brief A dynamically generated message based on conditions.
 *
 * This struct provides JSON-configurable conditional messages for effects.
 * It supports:
 * - Simple string messages
 * - Random selection from arrays
 * - Conditional branching with "yes"/"no"
 * - Message chaining with "and"
 * - Messages with side effects
 *
 * The JSON format supports:
 * - Simple: "message text"
 * - Random: ["option1", "option2", ...]
 * - Conditional: { "condition": value, "yes": "msg", "no": "msg" }
 * - Chained: { "and": ["msg1", "msg2", ...] }
 * - With effect: { "message": "text", "effect": {...} }
 */
struct effect_message_t {
    private:
        std::function<std::string( const effect_context & )> function;

    public:
        effect_message_t() = default;
        effect_message_t( const std::string &msg );
        effect_message_t( const JsonObject &jo );
        effect_message_t( const JsonArray &ja );
        static effect_message_t from_member( const JsonObject &jo, const std::string &member_name );

        /**
         * Evaluate the dynamic message with the given context.
         * @param ctx The effect context containing character and effect info.
         * @return The generated message string, or empty if no message.
         */
        std::string operator()( const effect_context &ctx ) const {
            if( !function ) {
                return std::string{};
            }
            return function( ctx );
        }

        /** Returns true if this message has a valid function. */
        bool is_valid() const {
            return static_cast<bool>( function );
        }
};

/**
 * @brief Functor for effect-triggered actions.
 *
 * This allows effects to trigger actions (like modifying stored kcal)
 * when their dynamic messages are displayed.
 */
struct effect_fun_t {
    private:
        std::function<void( const effect_context & )> function;

    public:
        effect_fun_t() = default;
        effect_fun_t( std::function<void( const effect_context & )> func )
            : function( std::move( func ) ) {}

        void set_mod_stored_kcal( int amount );

        void operator()( const effect_context &ctx ) const {
            if( function ) {
                function( ctx );
            }
        }
};

