/*
===========================================================================
Copyright (C) Nocturne contributors

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

// Nocturne の層：窓（nocturne_glimp.c）と入力（nocturne_input.c）が分け合うもの。

#pragma once

#include <nocturne/core.h>
#include <nocturne/ui.h>

typedef struct {
	struct ui_window	*win;		// VKimp_Init の前と VKimp_Shutdown の後は NULL
	int			width, height;		// 最後の UI_EVENT_KIND_RESIZE が言った窓の客の面の大きさ
	qboolean	isFullscreen;
} nocturneWindow_t;

extern nocturneWindow_t nwin;

extern cvar_t *in_nograb;

void IN_Init( void );
void IN_Frame( void );
void IN_Shutdown( void );

// 窓の事象一つを Quake の事象と窓の状態にする（HandleEvents が ui_window_event の事象ごとに呼ぶ）。
void Nocturne_WindowEvent( const struct ui_event *e );
