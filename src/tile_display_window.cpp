#if defined(TILES)

#include "tile_display_window.h"

#include "cata_tiles.h"
#include "cursesport.h"
#include "game.h"
#include "input.h"
#include "options.h"
#include "output.h"
#include "sdltiles.h"
#include "ui_manager.h"

#include <algorithm>

namespace
{

/**
 * Adapter class to access protected members of cata_tiles.
 */
class tile_display_adapter : public cata_tiles
{
    public:
        static tile_display_adapter *convert( cata_tiles *ct ) {
            return static_cast<tile_display_adapter *>( ct );
        }

        void draw_tile_layer( const std::string &tile_id, const point &p,
                              std::optional<SDL_Color> tint, int rotation ) {
            int height_3d = 0;
            const tile_search_params tile{ tile_id, C_NONE, "", -1, rotation };
            draw_from_id_string(
                tile, tripoint( p, 0 ), std::nullopt, tint,
                lit_level::BRIGHT, false, 0, true, height_3d );
        }

        bool has_tile( const std::string &tile_id ) {
            const tile_search_params tile{ tile_id, C_NONE, "", -1, 0 };
            return tile_type_search( tile ).has_value();
        }

        void advance_animation_frame() {
            idle_animations.set_enabled( get_option<bool>( "ANIMATIONS" ) );
            idle_animations.prepare_for_redraw();
        }
};

} // namespace

tile_display_window::~tile_display_window()
{
    if( saved_original_zoom_ && tilecontext ) {
        tilecontext->set_draw_scale( static_cast<int>( original_zoom_ ) );
    }
}

void tile_display_window::set_position( int x, int y )
{
    pos_x_ = x;
    pos_y_ = y;
}

void tile_display_window::clear_layers()
{
    layers_.clear();
}

void tile_display_window::add_layer( const tile_display_layer &layer )
{
    layers_.push_back( layer );
}

void tile_display_window::add_layer( const std::string &tile_id )
{
    layers_.emplace_back( tile_id );
}

void tile_display_window::add_layer_with_tint( const std::string &tile_id, uint8_t r, uint8_t g,
        uint8_t b, uint8_t a )
{
    SDL_Color tint{ r, g, b, a };
    layers_.emplace_back( tile_id, tint, 0 );
}

void tile_display_window::add_layer_rotated( const std::string &tile_id, int rotation )
{
    layers_.emplace_back( tile_id, std::nullopt, rotation );
}

void tile_display_window::add_layer_full( const std::string &tile_id, uint8_t r, uint8_t g,
        uint8_t b, uint8_t a, int rotation )
{
    SDL_Color tint{ r, g, b, a };
    layers_.emplace_back( tile_id, tint, rotation );
}

void tile_display_window::set_zoom( float zoom_level )
{
    zoom_ = std::clamp( zoom_level, MIN_ZOOM, MAX_ZOOM );
}

void tile_display_window::draw_tiles( const point &pixel_pos ) const
{
    if( !tilecontext ) {
        return;
    }

    tile_display_adapter *adapter = tile_display_adapter::convert( &*tilecontext );

    for( const tile_display_layer &layer : layers_ ) {
        adapter->draw_tile_layer( layer.tile_id, pixel_pos, layer.tint, layer.rotation );
    }
}

std::string tile_display_window::query()
{
    if( !tilecontext ) {
        return "ERROR";
    }

    // Save original zoom
    if( !saved_original_zoom_ ) {
        original_zoom_ = DEFAULT_TILESET_ZOOM;
        saved_original_zoom_ = true;
    }

    tile_display_adapter *adapter = tile_display_adapter::convert( &*tilecontext );

    // Set up input context
    input_context ctxt( "TILE_DISPLAY_WINDOW" );
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "CONFIRM" );
    ctxt.register_action( "LEFT" );
    ctxt.register_action( "RIGHT" );
    ctxt.register_action( "UP" );
    ctxt.register_action( "DOWN" );

    // Set timeout for animations (125ms like the main game loop)
    ctxt.set_timeout( ANIMATION_TIMEOUT_MS );

    // Get pixel conversion factors for position
    const int termx_pixels = projected_window_width() / TERMX;
    const int termy_pixels = projected_window_height() / TERMY;

    std::string action;
    do {
        // Set zoom for this frame
        tilecontext->set_draw_scale( static_cast<int>( zoom_ * DEFAULT_TILESET_ZOOM ) );

        // Get current tile dimensions in pixels (after zoom)
        const int t_width = tilecontext->get_tile_width();
        const int t_height = tilecontext->get_tile_height();

        // Calculate position in pixels
        // Position is where the tile's top-left corner goes
        int pixel_x;
        int pixel_y;
        if( pos_x_ < 0 ) {
            // Center horizontally based on tile size
            pixel_x = ( projected_window_width() - t_width ) / 2;
        } else {
            pixel_x = pos_x_ * termx_pixels;
        }
        if( pos_y_ < 0 ) {
            // Center vertically based on tile size
            pixel_y = ( projected_window_height() - t_height ) / 2;
        } else {
            pixel_y = pos_y_ * termy_pixels;
        }

        // Advance animation frame
        adapter->advance_animation_frame();

        // Draw all tile layers
        draw_tiles( point( pixel_x, pixel_y ) );

        // Force the display to update
        refresh_display();

        // Handle input
        action = ctxt.handle_input();

    } while( action == "TIMEOUT" );

    // Restore original zoom
    if( saved_original_zoom_ ) {
        tilecontext->set_draw_scale( static_cast<int>( original_zoom_ ) );
        saved_original_zoom_ = false;
    }

    return action;
}

bool tile_display_window::tile_exists( const std::string &tile_id )
{
    if( !tilecontext ) {
        return false;
    }

    tile_display_adapter *adapter = tile_display_adapter::convert( &*tilecontext );
    return adapter->has_tile( tile_id );
}

void tile_display_window::clear()
{
    layers_.clear();
    zoom_ = DEFAULT_ZOOM;
    pos_x_ = -1;
    pos_y_ = -1;

    if( saved_original_zoom_ && tilecontext ) {
        tilecontext->set_draw_scale( static_cast<int>( original_zoom_ ) );
        saved_original_zoom_ = false;
    }
}

#endif // TILES
