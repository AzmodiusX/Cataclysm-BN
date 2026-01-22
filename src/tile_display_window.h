#pragma once
#ifndef CATA_SRC_TILE_DISPLAY_WINDOW_H
#define CATA_SRC_TILE_DISPLAY_WINDOW_H

#if defined(TILES)

#include <optional>
#include <string>
#include <vector>

#include "color.h"
#include "cursesdef.h"
#include "point.h"
#include "sdl_wrappers.h"

/**
 * Represents a single tile layer to be rendered.
 * Layers are drawn in order, with later layers on top.
 */
struct tile_display_layer {
    // The tile ID to look up (e.g., "mon_zombie", "overlay_worn_backpack", "t_floor")
    std::string tile_id;
    // Optional tint color applied to this layer
    std::optional<SDL_Color> tint;
    // Rotation value (0=N, 1=W, 2=S, 3=W, 4=flip horizontal)
    int rotation = 0;

    tile_display_layer() = default;
    explicit tile_display_layer( const std::string &id ) : tile_id( id ) {}
    tile_display_layer( const std::string &id, std::optional<SDL_Color> t, int rot = 0 )
        : tile_id( id ), tint( t ), rotation( rot ) {}
};

/**
 * A window that renders arbitrary tiles from the tileset.
 *
 * This class allows Lua scripts (and C++ code) to display animated tile graphics
 * in a dedicated window. It supports:
 * - Rendering any tile by its ID (uses the existing tile lookup system)
 * - Multiple layers for compositing (similar to character overlays)
 * - Tinting individual layers with colors
 * - Animations (tiles with "animated": true cycle through their fg frames)
 * - Zoom control
 *
 * Tile IDs follow the same format as the tileset JSON:
 * - "mon_zombie" for monsters
 * - "t_floor" for terrain
 * - "overlay_worn_backpack" for overlays
 * - Custom IDs defined in tileset mods
 *
 * For animated tiles, the "fg" array defines frames and the "weight" of each
 * frame determines how many animation ticks it displays for.
 */
class tile_display_window
{
    public:
        tile_display_window() = default;
        ~tile_display_window();

        /**
         * Set desired window position in terminal cells.
         * @param x X position in terminal columns (-1 for centered)
         * @param y Y position in terminal rows (-1 for centered)
         */
        void set_position( int x, int y );

        /**
         * Clear all tile layers
         */
        void clear_layers();

        /**
         * Add a tile layer to be rendered.
         * Layers are drawn in the order they are added.
         * @param layer The layer configuration
         */
        void add_layer( const tile_display_layer &layer );

        /**
         * Add a tile layer by ID only (no tint, no rotation)
         */
        void add_layer( const std::string &tile_id );

        /**
         * Add a tile layer with a tint color
         * @param tile_id The tile ID
         * @param r Red component (0-255)
         * @param g Green component (0-255)
         * @param b Blue component (0-255)
         * @param a Alpha component (0-255, default 255)
         */
        void add_layer_with_tint( const std::string &tile_id, uint8_t r, uint8_t g, uint8_t b,
                                  uint8_t a = 255 );

        /**
         * Add a tile layer with rotation
         * @param tile_id The tile ID
         * @param rotation Rotation value (0=N, 1=W, 2=S, 3=E, 4=flip horizontal)
         */
        void add_layer_rotated( const std::string &tile_id, int rotation );

        /**
         * Add a tile layer with tint and rotation
         * @param tile_id The tile ID
         * @param r Red component (0-255)
         * @param g Green component (0-255)
         * @param b Blue component (0-255)
         * @param a Alpha component (0-255)
         * @param rotation Rotation value (0=N, 1=W, 2=S, 3=E, 4=flip horizontal)
         */
        void add_layer_full( const std::string &tile_id, uint8_t r, uint8_t g, uint8_t b,
                             uint8_t a, int rotation );

        /**
         * Set the zoom level (affects tile display size)
         * Default is 1.0, range is typically 0.25 to 4.0
         */
        void set_zoom( float zoom_level );

        /**
         * Display the window and wait for user to close it (ESC or q).
         * This is a blocking call that handles its own input loop.
         * If zoom is enabled, +/- keys will zoom in/out.
         * Animations will update automatically.
         * @return The action that closed the window ("QUIT" for ESC/q)
         */
        std::string query();

        /**
         * Check if a tile ID exists in the current tileset
         * @param tile_id The tile ID to check
         * @return true if the tile exists
         */
        static bool tile_exists( const std::string &tile_id );

        /**
         * Clear all layers and reset settings
         */
        void clear();

    private:
        // Draw the tile layers at the given position
        void draw_tiles( const point &pos ) const;

        std::vector<tile_display_layer> layers_;
        float zoom_ = 1.0f;
        int pos_x_ = -1;            // -1 = centered
        int pos_y_ = -1;            // -1 = centered
        float original_zoom_ = 0.0f;
        bool saved_original_zoom_ = false;

        static constexpr float MIN_ZOOM = 0.25f;
        static constexpr float MAX_ZOOM = 4.0f;
        static constexpr float DEFAULT_ZOOM = 1.0f;
        static constexpr int ANIMATION_TIMEOUT_MS = 125;
};

#endif // TILES

#endif // CATA_SRC_TILE_DISPLAY_WINDOW_H
