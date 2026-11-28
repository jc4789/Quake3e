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
// 窓は UI_WINDOW_FLAGS_FRAME_QUEUE | UI_WINDOW_FLAGS_APP_SURFACES：面は Vulkan の swapchain が据え（n_vulkan_surface_create）、
// present は窓の面の提出（nocturne.ui の surfaces）になる。全画面の窓の大きさは画面の大きさで、UI_EVENT_KIND_RESIZE が
// 知らせる。全画面の窓は面を直に出すよう頼む（ui_window_direct）。

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "../client/client.h"

#include "nocturne_local.h"

#define WINDOW_TITLE "Quake3e"
#define FULLSCREEN_WAIT_MS 3000

// vkGetInstanceProcAddr は Vulkan のローダー（user/runtime/src/native/vulkan.c）の関数で、中核のライブラリー core.NLIB のエクスポート。
extern PFN_vkVoidFunction vkGetInstanceProcAddr( VkInstance instance, const char *pName );

nocturneWindow_t nwin;


/*
===============
GLW_WaitFullscreenSize

合成器は全画面の頼みに画面の大きさの UI_EVENT_KIND_RESIZE で答える。ほかの事象は順を保つ。
===============
*/
static qboolean GLW_WaitFullscreenSize( void )
{
	uint64_t deadline = n_clock_after( (uint64_t)FULLSCREEN_WAIT_MS * 1000000 );
	struct ui_event e = { .bytes = sizeof( e ) };

	while ( ui_window_event( nwin.win, &e, deadline ) >= 0 ) {
		Nocturne_WindowEvent( &e );
		if ( e.kind == UI_EVENT_KIND_RESIZE )
			return qtrue;
	}

	return qfalse;
}


// 窓の客の面の大きさを nwin に読む。
static void GLW_ReadSize( void )
{
	struct ui_window_status status = { .bytes = sizeof( status ) };

	if ( ui_window_get_status( nwin.win, &status ) >= 0 ) {
		nwin.width = (int)status.width;
		nwin.height = (int)status.height;
	}
}


static qboolean GLW_OpenWindow( glconfig_t *config, int mode, const char *modeFS, qboolean fullscreen )
{
	int desktopWidth = 0, desktopHeight = 0;
	ui_window_flags flags = UI_WINDOW_FLAGS_FRAME_QUEUE | UI_WINDOW_FLAGS_APP_SURFACES;
	struct ui_desktop_info desktop = { .bytes = sizeof( desktop ) };
	ui_result r;

	if ( ui_desktop( &desktop ) >= 0 ) {
		desktopWidth = (int)desktop.width;
		desktopHeight = (int)desktop.height;
	}

	config->isFullscreen = fullscreen;

	Com_Printf( "...setting mode %d:", mode );

	if ( !CL_GetModeInfo( &config->vidWidth, &config->vidHeight, &config->windowAspect, mode, modeFS, desktopWidth, desktopHeight, fullscreen ) ) {
		Com_Printf( " invalid mode\n" );
		return qfalse;
	}

	Com_Printf( " %d %d\n", config->vidWidth, config->vidHeight );

	if ( !fullscreen && r_noborder->integer )
		flags |= UI_WINDOW_FLAGS_NO_DECOR;

	r = ui_window_open( (uint32_t)config->vidWidth, (uint32_t)config->vidHeight, WINDOW_TITLE, sizeof( WINDOW_TITLE ) - 1,
		flags, NULL, &nwin.win );
	if ( r < 0 || !nwin.win ) {
		Com_Printf( "ui_window_open が失敗した：%i\n", (int)r );
		nwin.win = NULL;
		return qfalse;
	}

	nwin.width = config->vidWidth;
	nwin.height = config->vidHeight;
	GLW_ReadSize();
	nwin.isFullscreen = qfalse;

	// 合成器は窓を初めの提出で見せ、その時に前面にする（UI_EVENT_KIND_FOCUS）：まだ見せていない窓は最小化した窓では
	// なく、窓を出すには描かなければならない。
	gw_active = qtrue;
	gw_minimized = qfalse;

	return qtrue;
}


/*
===============
VKimp_Init

ウィンドウを開き、入力を受け、大きさをレンダラーに知らせる。
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
		// 画面が窓と同じ大きさなら UI_EVENT_KIND_RESIZE は来ない
		if ( ui_window_fullscreen( nwin.win, 1 ) >= 0 && ( GLW_WaitFullscreenSize() || ( ui_window_query_state( nwin.win ) & UI_WINDOW_STATE_FULLSCREEN ) ) ) {
			nwin.isFullscreen = qtrue;
			config->vidWidth = nwin.width;
			config->vidHeight = nwin.height;
			config->windowAspect = (float)nwin.width / (float)nwin.height;
			// 全画面の間、画面の核は合成せずに swapchain の面を画面へ直に出せる。出せない画面では何も変わらない。
			(void)ui_window_direct( nwin.win, 1 );
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
	n_result result;

	if ( !nwin.win )
		return qfalse;

	result = n_vulkan_surface_create( (size_t)instance, ui_window_id( nwin.win ) );
	if ( result < 0 ) {
		Com_Printf( "n_vulkan_surface_create が失敗した：%i\n", (int)result );
		return qfalse;
	}

	*surface = (VkSurfaceKHR)(uint64_t)result;
	return qtrue;
}


/*
===============
VKimp_Shutdown

レンダラーはこれより前に自分の面を壊している。
===============
*/
void VKimp_Shutdown( qboolean unloadDLL )
{
	IN_Shutdown();

	if ( nwin.win ) {
		ui_window_close( nwin.win );
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
	ui_result length = ui_clipboard_get( NULL, 0 ), got;

	if ( length <= 0 )
		return NULL;

	// 間に変わった中身は場所に入った分だけ読む
	data = Z_Malloc( (int)length + 1 );
	if ( ( got = ui_clipboard_get( data, (size_t)length ) ) < 0 ) {
		Z_Free( data );
		return NULL;
	}
	data[ got < length ? got : length ] = '\0';

	// 一行目だけ
	strtok( data, "\n\r\b" );
	return data;
}
