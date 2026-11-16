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

// The Nocturne platform layer: what the window (nocturne_glimp.c) and the input (nocturne_input.c) share.

#pragma once

#include <nocturne.h>

typedef struct {
	window_t	*win;				// NULL until VKimp_Init, and after VKimp_Shutdown
	int			width, height;		// the window's client size, as of the last EV_RESIZE
	qboolean	isFullscreen;
} nocturneWindow_t;

extern nocturneWindow_t nwin;

extern cvar_t *in_nograb;

void IN_Init( void );
void IN_Frame( void );
void IN_Shutdown( void );

// Turns one window event into Quake events and window state (HandleEvents calls it for each event win_event gives).
void Nocturne_WindowEvent( const struct gui_event *e );
