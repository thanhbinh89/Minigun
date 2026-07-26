#include "scr_minigun.h"

/*****************************************************************************/
/* MiniGun - 2 player turn based artillery duel.
 *
 * Skyline is a fixed set of edge-to-edge buildings across the panel.
 * Player 0 (human) stands on the leftmost building and fires rightward;
 * player 1 (alien) stands on the rightmost building and fires leftward.
 * See CLAUDE.md for the full game design.
 */
/*****************************************************************************/

struct minigun_building_t {
	uint8_t x;
	uint8_t w;
	uint8_t h; /* height above MINIGUN_GROUND_Y, in pixels */
};

/* Fixed skyline, widths sum to LCD_WIDTH (124px) so buildings tile edge to edge. */
static const minigun_building_t k_minigun_buildings[] = {
	{0,   18, 22},
	{18,  14, 34},
	{32,  20, 14},
	{52,  16, 26},
	{68,  10, 10},
	{78,  22, 30},
	{100, 10, 18},
	{110, 14, 24},
};
#define MINIGUN_BUILDING_COUNT	(sizeof(k_minigun_buildings) / sizeof(k_minigun_buildings[0]))

#define MINIGUN_GROUND_Y		(50) /* baseline all buildings sit on */

#define MINIGUN_ANGLE_MIN		(10)
#define MINIGUN_ANGLE_MAX		(80)
#define MINIGUN_ANGLE_STEP		(5)
#define MINIGUN_ANGLE_DEFAULT	(45)

#define MINIGUN_POWER_MIN		(0)
#define MINIGUN_POWER_MAX		(100)

enum minigun_state_t {
	MINIGUN_STATE_AIMING = 0,
	MINIGUN_STATE_FIRING,
	MINIGUN_STATE_ROUND_END,
	MINIGUN_STATE_GAME_OVER,
};

/* per-column building top y, precomputed once per match for O(1) collision lookup */
static uint8_t minigun_terrain_top[LCD_WIDTH];

static uint8_t minigun_player_x[2];
static uint8_t minigun_player_y[2];
static uint8_t minigun_angle_deg[2];
static uint8_t minigun_power_val[2];
static uint8_t minigun_current_player = 0;
static uint8_t minigun_game_state = MINIGUN_STATE_AIMING;
static bool	minigun_is_charging = false;

static void view_scr_minigun();

view_dynamic_t dyn_view_minigun = {
	{
		.item_type = ITEM_TYPE_DYNAMIC,
	},
	view_scr_minigun
};

view_screen_t scr_minigun = {
	&dyn_view_minigun,
	ITEM_NULL,
	ITEM_NULL,

	.focus_item = 0,
};

static void minigun_build_terrain() {
	uint8_t x = 0;
	for (uint8_t b = 0; b < MINIGUN_BUILDING_COUNT; b++) {
		uint8_t top = MINIGUN_GROUND_Y - k_minigun_buildings[b].h;
		for (uint8_t i = 0; i < k_minigun_buildings[b].w && x < LCD_WIDTH; i++, x++) {
			minigun_terrain_top[x] = top;
		}
	}
}

static void minigun_place_players() {
	const minigun_building_t& b_left  = k_minigun_buildings[0];
	const minigun_building_t& b_right = k_minigun_buildings[MINIGUN_BUILDING_COUNT - 1];

	minigun_player_x[0] = b_left.x + (b_left.w - MINIGUN_SPRITE_W) / 2;
	minigun_player_y[0] = MINIGUN_GROUND_Y - b_left.h - MINIGUN_SPRITE_H;

	minigun_player_x[1] = b_right.x + (b_right.w - MINIGUN_SPRITE_W) / 2;
	minigun_player_y[1] = MINIGUN_GROUND_Y - b_right.h - MINIGUN_SPRITE_H;
}

static void minigun_new_match() {
	minigun_build_terrain();
	minigun_place_players();

	for (uint8_t p = 0; p < 2; p++) {
		minigun_angle_deg[p] = MINIGUN_ANGLE_DEFAULT;
		minigun_power_val[p] = MINIGUN_POWER_MIN;
	}

	minigun_current_player = 0;
	minigun_is_charging	= false;
	minigun_game_state		= MINIGUN_STATE_AIMING;
}

static void minigun_draw_skyline() {
	for (uint8_t b = 0; b < MINIGUN_BUILDING_COUNT; b++) {
		const minigun_building_t& bd = k_minigun_buildings[b];
		uint8_t top = MINIGUN_GROUND_Y - bd.h;

		view_render.fillRect(bd.x, top, bd.w, bd.h, WHITE);

		/* windows: a grid of small black squares inset from the building edges */
		for (uint8_t wy = top + 3; (wy + 2) <= (MINIGUN_GROUND_Y - 2); wy += 6) {
			for (uint8_t wx = bd.x + 3; (wx + 2) <= (bd.x + bd.w - 2); wx += 6) {
				view_render.fillRect(wx, wy, 2, 2, BLACK);
			}
		}
	}
}

static void minigun_draw_hud() {
	view_render.setTextSize(1);
	view_render.setTextColor(WHITE);
	view_render.setCursor(0, LCD_HEIGHT - 8);
	view_render.print("Angle: ");
	view_render.print((int)minigun_angle_deg[minigun_current_player]);
	view_render.print(" Power: ");
	view_render.print((int)minigun_power_val[minigun_current_player]);
}

void view_scr_minigun() {
	minigun_draw_skyline();

	view_render.drawBitmap(minigun_player_x[0], minigun_player_y[0], bitmap_player_human, MINIGUN_SPRITE_W, MINIGUN_SPRITE_H, WHITE);
	view_render.drawBitmap(minigun_player_x[1], minigun_player_y[1], bitmap_player_alien, MINIGUN_SPRITE_W, MINIGUN_SPRITE_H, WHITE);

	minigun_draw_hud();
}

void scr_minigun_handle(ak_msg_t* msg) {
	switch (msg->sig) {
	case SCREEN_ENTRY: {
		APP_DBG_SIG("SCREEN_ENTRY\n");
		minigun_new_match();
	} break;

	case AC_DISPLAY_BUTON_UP_PRESSED: {
		APP_DBG_SIG("AC_DISPLAY_BUTON_UP_PRESSED\n");
		if (minigun_game_state == MINIGUN_STATE_AIMING) {
			if (minigun_angle_deg[minigun_current_player] + MINIGUN_ANGLE_STEP <= MINIGUN_ANGLE_MAX) {
				minigun_angle_deg[minigun_current_player] += MINIGUN_ANGLE_STEP;
			}
		}
	} break;

	case AC_DISPLAY_BUTON_DOWN_PRESSED: {
		APP_DBG_SIG("AC_DISPLAY_BUTON_DOWN_PRESSED\n");
		if (minigun_game_state == MINIGUN_STATE_AIMING) {
			if (minigun_angle_deg[minigun_current_player] >= MINIGUN_ANGLE_MIN + MINIGUN_ANGLE_STEP) {
				minigun_angle_deg[minigun_current_player] -= MINIGUN_ANGLE_STEP;
			}
		}
	} break;

	default:
		break;
	}
}
