#ifndef __SCREENS_BITMAP_H__
#define __SCREENS_BITMAP_H__

#include "view_render.h"

// scr_welcome
extern const unsigned char PROGMEM bitmap_dolphin[];

// scr_minigun
#define MINIGUN_SPRITE_W (8)
#define MINIGUN_SPRITE_H (11)
extern const unsigned char PROGMEM bitmap_player_human[];
extern const unsigned char PROGMEM bitmap_player_alien[];

#endif //__SCREENS_BITMAP_H__
