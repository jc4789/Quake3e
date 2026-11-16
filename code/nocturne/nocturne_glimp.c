/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.
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

// Nocturne の窓と Vulkan の surface。
// 窓は WIN_FRAME_QUEUE | WIN_APP_SURFACES：面は Vulkan の swapchain が据え（n_vulkan_surface_create）、present は
// SYS_WIN_SUBMIT になる（common/display_abi.h の窓の節）。全画面の窓の大きさは画面の大きさで、EV_RESIZE が知らせる。

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "../client/client.h"

#include <nocturne.h>
#include <nocturne_gpu.h>
#include "nocturne_local.h"

#define WINDOW_TITLE "Quake3e"
#define FULLSCREEN_WAIT_MS 3000

// vkGetInstanceProcAddr is the loader's, linked into the C library (user/runtime/src/native/vulkan.c).
extern PFN_vkVoidFunction vkGetInstanceProcAddr( VkInstance instance, const char *pName );

nocturneWindow_t nwin;


/*
===============
GLW_WaitFullscreenSize

The window system answers a fullscreen request with the screen's size in EV_RESIZE. Other events keep their order.
===============
*/
static qboolean GLW_WaitFullscreenSize( void )
{
	uint64_t deadline = n_clock_after( (uint64_t)FULLSCREEN_WAIT_MS * N_NS_PER_MS );
	struct gui_event e;

	while ( win_event( nwin.win, &e, deadline ) > 0 ) {
		Nocturne_WindowEvent( &e );
		if ( e.type == EV_RESIZE )
			return qtrue;
	}

	return qfalse;
}


static qboolean GLW_OpenWindow( glconfig_t *config, int mode, const char *modeFS, qboolean fullscreen )
{
	int desktopWidth, desktopHeight;
	unsigned flags = WIN_FRAME_QUEUE | WIN_APP_SURFACES;
	n_result r;

	if ( screen_size( &desktopWidth, &desktopHeight ) < 0 ) {
		desktopWidth = 0;
		desktopHeight = 0;
	}

	config->isFullscreen = fullscreen;

	Com_Printf( "...setting mode %d:", mode );

	if ( !CL_GetModeInfo( &config->vidWidth, &config->vidHeight, &config->windowAspect, mode, modeFS, desktopWidth, desktopHeight, fullscreen ) ) {
		Com_Printf( " invalid mode\n" );
		return qfalse;
	}

	Com_Printf( " %d %d\n", config->vidWidth, config->vidHeight );

	if ( !fullscreen && r_noborder->integer )
		flags |= WIN_NO_DECOR;

	r = win_open( config->vidWidth, config->vidHeight, WINDOW_TITLE, (int)flags, &nwin.win );
	if ( r < 0 || !nwin.win ) {
		Com_Printf( "win_open failed: %i\n", (int)r );
		nwin.win = NULL;
		return qfalse;
	}

	nwin.width = nwin.win->w;
	nwin.height = nwin.win->h;
	nwin.isFullscreen = qfalse;

	// The window system shows a window with its first present and gives it the focus then (EV_FOCUS):
	// a window not yet shown is not a minimized one, and the renderer must draw to make it appear.
	gw_active = qtrue;
	gw_minimized = qfalse;

	return qtrue;
}


/*
===============
VKimp_Init

Opens the window, takes the input, and reports the size to the renderer.
===============
*/
void VKimp_Init( glconfig_t *config )
{
	Com_DPrintf( "VKimp_Init()\n" );

	r_swapInterval = Cvar_Get( "r_swapInterval", "0", CVAR_ARCHIVE | CVAR_LATCH );

	config->deviceSupportsGamma = qfalse;
	config->stereoEnabled = qfalse;

	if ( !GLW_OpenWindow( config, r_mode->integer, r_modeFullscreen->string, r_fullscreen->integer ? qtrue : qfalse ) ) {
		Com_Printf( "Setting r_mode %d failed, falling back on r_mode %d\n", r_mode->integer, 3 );

		if ( !GLW_OpenWindow( config, 3, "", r_fullscreen->integer ? qtrue : qfalse ) )
			Com_Error( ERR_FATAL, "VKimp_Init() - could not open a window" );
	}

	// These values force the UI to disable driver selection
	config->driverType = GLDRV_ICD;
	config->hardwareType = GLHW_GENERIC;

	IN_Init();

	if ( config->isFullscreen ) {
		// no EV_RESIZE comes when the screen has the size the window had
		if ( win_fullscreen( nwin.win, true ) >= 0 && ( GLW_WaitFullscreenSize() || ( win_state( nwin.win ) & WIN_STATE_FULLSCREEN ) ) ) {
			nwin.isFullscreen = qtrue;
			config->vidWidth = nwin.width;
			config->vidHeight = nwin.height;
			config->windowAspect = (float)nwin.width / (float)nwin.height;
		} else {
			Com_Printf( "...WARNING: fullscreen unavailable\n" );
			config->isFullscreen = qfalse;
		}
	}

	HandleEvents();

	Key_ClearStates();
}


/*
===============
VK_GetInstanceProcAddr
===============
*/
void *VK_GetInstanceProcAddr( VkInstance instance, const char *name )
{
	return (void *)vkGetInstanceProcAddr( instance, name );
}


/*
===============
VK_CreateSurface
===============
*/
qboolean VK_CreateSurface( VkInstance instance, VkSurfaceKHR *surface )
{
	int32_t result;

	if ( !nwin.win )
		return qfalse;

	result = n_vulkan_surface_create( (struct VkInstance_T *)instance, nwin.win->handle, (struct VkSurfaceKHR_T **)surface );
	if ( result != VK_SUCCESS ) {
		Com_Printf( "n_vulkan_surface_create failed: %i\n", (int)result );
		return qfalse;
	}

	return qtrue;
}


/*
===============
VKimp_Shutdown

The renderer has destroyed its surface before this.
===============
*/
void VKimp_Shutdown( qboolean unloadDLL )
{
	IN_Shutdown();

	if ( nwin.win ) {
		win_close( nwin.win );
		nwin.win = NULL;
	}

	nwin.isFullscreen = qfalse;
	gw_active = qfalse;
	gw_minimized = qtrue;

	(void)unloadDLL;
}


/*
===============
Sys_GetClipboardData
===============
*/
char *Sys_GetClipboardData( void )
{
	char *data;
	n_result length = clipboard_get( NULL, 0 );

	if ( length <= 0 )
		return NULL;

	data = Z_Malloc( (int)length + 1 );
	if ( clipboard_get( data, (size_t)length ) < 0 ) {
		Z_Free( data );
		return NULL;
	}
	data[ length ] = '\0';

	// the first line only
	strtok( data, "\n\r\b" );
	return data;
}
