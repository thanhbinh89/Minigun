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

/* Skyline is regenerated per match (minigun_generate_skyline) so every game
 * looks different; widths always sum to exactly LCD_WIDTH (124px) so
 * buildings still tile edge to edge with no gaps.
 */
#define MINIGUN_BUILDING_COUNT			(8)
#define MINIGUN_BUILDING_MIN_W			(12) /* >= sprite width + margin, so players fit centered */
#define MINIGUN_BUILDING_HEIGHT_MIN	(8)
#define MINIGUN_BUILDING_HEIGHT_MAX	(36) /* stays well clear of the top of the panel */

static minigun_building_t minigun_buildings[MINIGUN_BUILDING_COUNT];

#define MINIGUN_GROUND_Y		(50) /* baseline all buildings sit on */

#define MINIGUN_AIM_LINE_LEN	(6) /* short aim indicator drawn off the current shooter */

#define MINIGUN_ANGLE_MIN		(10)
#define MINIGUN_ANGLE_MAX		(80)
#define MINIGUN_ANGLE_STEP		(5)
#define MINIGUN_ANGLE_DEFAULT	(45)

#define MINIGUN_POWER_MIN			(0)
#define MINIGUN_POWER_MAX			(100)
#define MINIGUN_POWER_CHARGE_STEP	(3)

#define MINIGUN_SPEED_MIN		(1.5f)	/* px/tick at power=0 */
#define MINIGUN_SPEED_MAX		(6.0f)	/* px/tick at power=100 */
#define MINIGUN_GRAVITY			(0.28f)	/* px/tick^2 */
#define MINIGUN_DEG2RAD			(0.0174533f)

#define MINIGUN_TRAIL_LEN			(10)	/* short comet trail, not the full flight path */
#define MINIGUN_MAX_FLIGHT_TICKS	(150)	/* safety cap so a shot always resolves */

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
static bool	minigun_pending_win = false;
static uint8_t minigun_winner = 0;

/* in-flight projectile */
static float	minigun_proj_x, minigun_proj_y;
static float	minigun_proj_vx, minigun_proj_vy;
static uint16_t minigun_flight_ticks;

/* short comet-style trail behind the projectile, ring buffer */
struct minigun_trail_point_t {
	uint8_t x;
	uint8_t y;
};
static minigun_trail_point_t minigun_trail[MINIGUN_TRAIL_LEN];
static uint8_t minigun_trail_count = 0;

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

/* Random skyline for this match: every building starts at the minimum
 * width, then the leftover width (LCD_WIDTH - COUNT*MIN_W) is handed out
 * one pixel at a time to random buildings - guarantees the widths sum to
 * exactly LCD_WIDTH regardless of how the RNG lands. Heights are just a
 * uniform random pick per building.
 */
static void minigun_generate_skyline() {
	srand(sys_ctrl_millis());

	uint8_t bonus = LCD_WIDTH - (MINIGUN_BUILDING_COUNT * MINIGUN_BUILDING_MIN_W);

	for (uint8_t b = 0; b < MINIGUN_BUILDING_COUNT; b++) {
		minigun_buildings[b].w = MINIGUN_BUILDING_MIN_W;
		minigun_buildings[b].h = MINIGUN_BUILDING_HEIGHT_MIN + \
			(rand() % (MINIGUN_BUILDING_HEIGHT_MAX - MINIGUN_BUILDING_HEIGHT_MIN + 1));
	}

	for (uint8_t i = 0; i < bonus; i++) {
		minigun_buildings[rand() % MINIGUN_BUILDING_COUNT].w++;
	}

	uint8_t x = 0;
	for (uint8_t b = 0; b < MINIGUN_BUILDING_COUNT; b++) {
		minigun_buildings[b].x = x;
		x += minigun_buildings[b].w;
	}
}

static void minigun_build_terrain() {
	uint8_t x = 0;
	for (uint8_t b = 0; b < MINIGUN_BUILDING_COUNT; b++) {
		uint8_t top = MINIGUN_GROUND_Y - minigun_buildings[b].h;
		for (uint8_t i = 0; i < minigun_buildings[b].w && x < LCD_WIDTH; i++, x++) {
			minigun_terrain_top[x] = top;
		}
	}
}

static void minigun_place_players() {
	const minigun_building_t& b_left  = minigun_buildings[0];
	const minigun_building_t& b_right = minigun_buildings[MINIGUN_BUILDING_COUNT - 1];

	minigun_player_x[0] = b_left.x + (b_left.w - MINIGUN_SPRITE_W) / 2;
	minigun_player_y[0] = MINIGUN_GROUND_Y - b_left.h - MINIGUN_SPRITE_H;

	minigun_player_x[1] = b_right.x + (b_right.w - MINIGUN_SPRITE_W) / 2;
	minigun_player_y[1] = MINIGUN_GROUND_Y - b_right.h - MINIGUN_SPRITE_H;
}

static void minigun_trail_reset() {
	minigun_trail_count = 0;
}

static void minigun_trail_push(uint8_t x, uint8_t y) {
	if (minigun_trail_count < MINIGUN_TRAIL_LEN) {
		minigun_trail[minigun_trail_count].x = x;
		minigun_trail[minigun_trail_count].y = y;
		minigun_trail_count++;
	}
	else {
		for (uint8_t i = 1; i < MINIGUN_TRAIL_LEN; i++) {
			minigun_trail[i - 1] = minigun_trail[i];
		}
		minigun_trail[MINIGUN_TRAIL_LEN - 1].x = x;
		minigun_trail[MINIGUN_TRAIL_LEN - 1].y = y;
	}
}

static void minigun_resolve_round(bool hit) {
	timer_remove_attr(AC_TASK_DISPLAY_ID, AC_MINIGUN_PROJECTILE_TICK);

	minigun_pending_win = hit;
	minigun_winner		 = minigun_current_player;
	minigun_game_state	 = MINIGUN_STATE_ROUND_END;

	BUZZER_PlaySound(hit ? BUZZER_SOUND_HIGHSCORE : BUZZER_SOUND_CLICK);

	timer_set(AC_TASK_DISPLAY_ID, AC_MINIGUN_ROUND_END_TICK, AC_MINIGUN_ROUND_END_DELAY_MS, TIMER_ONE_SHOT);
}

/* One simulation step of the in-flight shot: integrate gravity, leave a trail
 * dot, then check (in priority order) whether it reached the opponent, the
 * skyline, or the edge of the panel. Terrain/off-screen/timeout all resolve
 * as a miss; only touching the opponent's sprite ends the match.
 */
static void minigun_step_projectile() {
	minigun_proj_x	+= minigun_proj_vx;
	minigun_proj_y	+= minigun_proj_vy;
	minigun_proj_vy	+= MINIGUN_GRAVITY;
	minigun_flight_ticks++;

	bool off_screen = (minigun_proj_x < 0.0f || minigun_proj_x >= (float)LCD_WIDTH || minigun_proj_y >= (float)LCD_HEIGHT);

	if (!off_screen && minigun_proj_y >= 0.0f) {
		minigun_trail_push((uint8_t)minigun_proj_x, (uint8_t)minigun_proj_y);
	}

	uint8_t opp = 1 - minigun_current_player;
	bool hit_opponent = false;
	if (!off_screen) {
		hit_opponent = (minigun_proj_x >= (float)minigun_player_x[opp] - 1 &&
						minigun_proj_x <= (float)(minigun_player_x[opp] + MINIGUN_SPRITE_W) &&
						minigun_proj_y >= (float)minigun_player_y[opp] - 1 &&
						minigun_proj_y <= (float)(minigun_player_y[opp] + MINIGUN_SPRITE_H));
	}

	bool hit_terrain = false;
	if (!off_screen && minigun_proj_y >= 0.0f) {
		uint8_t ix = (uint8_t)minigun_proj_x;
		if (minigun_proj_y >= (float)minigun_terrain_top[ix]) {
			hit_terrain = true;
		}
	}

	if (hit_opponent) {
		minigun_resolve_round(true);
	}
	else if (hit_terrain || off_screen || minigun_flight_ticks >= MINIGUN_MAX_FLIGHT_TICKS) {
		minigun_resolve_round(false);
	}
}

static void minigun_fire_shot() {
	float rad	= minigun_angle_deg[minigun_current_player] * MINIGUN_DEG2RAD;
	float speed = MINIGUN_SPEED_MIN + (minigun_power_val[minigun_current_player] / (float)MINIGUN_POWER_MAX) * (MINIGUN_SPEED_MAX - MINIGUN_SPEED_MIN);
	float dir	= (minigun_current_player == 0) ? 1.0f : -1.0f;

	minigun_proj_x	= minigun_player_x[minigun_current_player] + (MINIGUN_SPRITE_W / 2);
	minigun_proj_y	= minigun_player_y[minigun_current_player] - 1;
	minigun_proj_vx = speed * cos(rad) * dir;
	minigun_proj_vy = -(speed * sin(rad));

	minigun_trail_reset();
	minigun_flight_ticks = 0;
	minigun_game_state	  = MINIGUN_STATE_FIRING;

	BUZZER_PlaySound(BUZZER_SOUND_BANG);

	timer_set(AC_TASK_DISPLAY_ID, AC_MINIGUN_PROJECTILE_TICK, AC_MINIGUN_PROJECTILE_TICK_INTERVAL_MS, TIMER_PERIODIC);
}

static void minigun_start_next_turn() {
	if (minigun_pending_win) {
		minigun_game_state = MINIGUN_STATE_GAME_OVER;
		return;
	}

	minigun_trail_reset();
	minigun_current_player = 1 - minigun_current_player;
	minigun_power_val[minigun_current_player] = MINIGUN_POWER_MIN;
	minigun_game_state = MINIGUN_STATE_AIMING;
}

static void minigun_new_match() {
	minigun_generate_skyline();
	minigun_build_terrain();
	minigun_place_players();

	for (uint8_t p = 0; p < 2; p++) {
		minigun_angle_deg[p] = MINIGUN_ANGLE_DEFAULT;
		minigun_power_val[p] = MINIGUN_POWER_MIN;
	}

	minigun_current_player = 0;
	minigun_is_charging	= false;
	minigun_pending_win	= false;
	minigun_flight_ticks	= 0;
	minigun_game_state		= MINIGUN_STATE_AIMING;
	minigun_trail_reset();

	timer_remove_attr(AC_TASK_DISPLAY_ID, AC_MINIGUN_CHARGE_TICK);
	timer_remove_attr(AC_TASK_DISPLAY_ID, AC_MINIGUN_PROJECTILE_TICK);
	timer_remove_attr(AC_TASK_DISPLAY_ID, AC_MINIGUN_ROUND_END_TICK);
}

static void minigun_draw_skyline() {
	for (uint8_t b = 0; b < MINIGUN_BUILDING_COUNT; b++) {
		const minigun_building_t& bd = minigun_buildings[b];
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

/* Short dash off the current shooter showing their aim, pivoting from
 * about shoulder height in the direction/angle they're about to fire.
 */
static void minigun_draw_aim_line() {
	uint8_t p = minigun_current_player;
	float rad = minigun_angle_deg[p] * MINIGUN_DEG2RAD;
	float dir = (p == 0) ? 1.0f : -1.0f;

	int16_t ox = minigun_player_x[p] + (MINIGUN_SPRITE_W / 2);
	int16_t oy = minigun_player_y[p] + 5;
	int16_t tx = ox + (int16_t)(dir * MINIGUN_AIM_LINE_LEN * cos(rad));
	int16_t ty = oy - (int16_t)(MINIGUN_AIM_LINE_LEN * sin(rad));

	view_render.drawLine(ox, oy, tx, ty, WHITE);
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

static void minigun_draw_game_over() {
	view_render.setTextSize(1);
	view_render.setTextColor(WHITE);

	view_render.setCursor(16, 14);
	view_render.print("PLAYER ");
	view_render.print((int)(minigun_winner + 1));

	view_render.setCursor(34, 26);
	view_render.print("WINS!");

	view_render.setCursor(4, 44);
	view_render.print("MODE: play again");
}

void view_scr_minigun() {
	if (minigun_game_state == MINIGUN_STATE_GAME_OVER) {
		minigun_draw_game_over();
		return;
	}

	minigun_draw_skyline();

	view_render.drawBitmap(minigun_player_x[0], minigun_player_y[0], bitmap_player_human, MINIGUN_SPRITE_W, MINIGUN_SPRITE_H, WHITE);
	view_render.drawBitmap(minigun_player_x[1], minigun_player_y[1], bitmap_player_alien, MINIGUN_SPRITE_W, MINIGUN_SPRITE_H, WHITE);
	minigun_draw_aim_line();

	for (uint8_t i = 0; i < minigun_trail_count; i++) {
		view_render.drawPixel(minigun_trail[i].x, minigun_trail[i].y, WHITE);
	}

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

	case AC_DISPLAY_BUTON_MODE_PRESSED: {
		APP_DBG_SIG("AC_DISPLAY_BUTON_MODE_PRESSED\n");
		if (minigun_game_state == MINIGUN_STATE_GAME_OVER) {
			BUZZER_PlaySound(BUZZER_SOUND_CLICK);
			minigun_new_match();
		}
		else if (minigun_game_state == MINIGUN_STATE_AIMING && !minigun_is_charging) {
			minigun_is_charging = true;
			minigun_power_val[minigun_current_player] = MINIGUN_POWER_MIN;
			timer_set(AC_TASK_DISPLAY_ID, AC_MINIGUN_CHARGE_TICK, AC_MINIGUN_CHARGE_TICK_INTERVAL_MS, TIMER_PERIODIC);
		}
	} break;

	case AC_MINIGUN_CHARGE_TICK: {
		uint8_t p = minigun_current_player;
		if (minigun_power_val[p] + MINIGUN_POWER_CHARGE_STEP >= MINIGUN_POWER_MAX) {
			minigun_power_val[p] = MINIGUN_POWER_MAX;
		}
		else {
			minigun_power_val[p] += MINIGUN_POWER_CHARGE_STEP;
		}
	} break;

	case AC_DISPLAY_BUTON_MODE_RELEASED: {
		APP_DBG_SIG("AC_DISPLAY_BUTON_MODE_RELEASED\n");
		if (minigun_game_state == MINIGUN_STATE_AIMING && minigun_is_charging) {
			minigun_is_charging = false;
			timer_remove_attr(AC_TASK_DISPLAY_ID, AC_MINIGUN_CHARGE_TICK);
			minigun_fire_shot();
		}
	} break;

	case AC_MINIGUN_PROJECTILE_TICK: {
		minigun_step_projectile();
	} break;

	case AC_MINIGUN_ROUND_END_TICK: {
		minigun_start_next_turn();
	} break;

	default:
		break;
	}
}
