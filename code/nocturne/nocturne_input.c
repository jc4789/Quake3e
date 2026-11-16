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

// Nocturne の入力：窓の事象（EV_KEY、EV_MOUSE_*、EV_WHEEL …）を Quake の事象（SE_KEY、SE_CHAR、SE_MOUSE）にする。
// 鍵は物理の usage（HID の Keyboard page）で K_* に結び、文字は配列で決まった EV_KEY の key（Unicode）から作る。
// 相対のマウスは窓の指の固定（win_pointer_lock）で、EV_MOUSE_MOVE の dx、dy が装置の生の量になる。

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "../client/client.h"
#include "../client/keycodes.h"

#include <nocturne.h>
#include "nocturne_local.h"

#define Com_QueueEvent Sys_QueEvent

#define CTRL(a) ((a)-'a'+1)

#define MAX_CONSOLE_KEYS 16

cvar_t *in_nograb;

static cvar_t *in_mouse;
static cvar_t *cl_consoleKeys;

static int in_eventTime;
static qboolean pointerLockWanted;	// what IN_Frame asked the window for
static qboolean pointerLocked;		// what EV_POINTER_LOCK last said
static uint32_t heldButtons;
static int wheelRemainder;
static uint32_t lastMods;

// Keyboard page usages (HID Usage Tables, 10) to Quake keys. 0: no Quake key.
static const int usageKeys[0xE8] = {
	[0x04] = K_A, [0x05] = K_B, [0x06] = K_C, [0x07] = K_D, [0x08] = K_E, [0x09] = K_F, [0x0A] = K_G,
	[0x0B] = K_H, [0x0C] = K_I, [0x0D] = K_J, [0x0E] = K_K, [0x0F] = K_L, [0x10] = K_M, [0x11] = K_N,
	[0x12] = K_O, [0x13] = K_P, [0x14] = K_Q, [0x15] = K_R, [0x16] = K_S, [0x17] = K_T, [0x18] = K_U,
	[0x19] = K_V, [0x1A] = K_W, [0x1B] = K_X, [0x1C] = K_Y, [0x1D] = K_Z,
	[0x1E] = '1', [0x1F] = '2', [0x20] = '3', [0x21] = '4', [0x22] = '5', [0x23] = '6', [0x24] = '7',
	[0x25] = '8', [0x26] = '9', [0x27] = '0',
	[0x28] = K_ENTER, [0x29] = K_ESCAPE, [0x2A] = K_BACKSPACE, [0x2B] = K_TAB, [0x2C] = K_SPACE,
	[0x2D] = K_MINUS, [0x2E] = K_EQUAL, [0x2F] = K_BRACKET_OPEN, [0x30] = K_BRACKET_CLOSE,
	[0x31] = K_BACKSLASH, [0x32] = K_BACKSLASH, [0x33] = K_SEMICOLON, [0x34] = K_QUOTE,
	[0x35] = K_CONSOLE, [0x36] = K_COMMA, [0x37] = K_DOT, [0x38] = K_SLASH, [0x39] = K_CAPSLOCK,
	[0x3A] = K_F1, [0x3B] = K_F2, [0x3C] = K_F3, [0x3D] = K_F4, [0x3E] = K_F5, [0x3F] = K_F6,
	[0x40] = K_F7, [0x41] = K_F8, [0x42] = K_F9, [0x43] = K_F10, [0x44] = K_F11, [0x45] = K_F12,
	[0x46] = K_PRINT, [0x47] = K_SCROLLOCK, [0x48] = K_PAUSE, [0x49] = K_INS, [0x4A] = K_HOME,
	[0x4B] = K_PGUP, [0x4C] = K_DEL, [0x4D] = K_END, [0x4E] = K_PGDN,
	[0x4F] = K_RIGHTARROW, [0x50] = K_LEFTARROW, [0x51] = K_DOWNARROW, [0x52] = K_UPARROW,
	[0x53] = K_KP_NUMLOCK, [0x54] = K_KP_SLASH, [0x55] = K_KP_STAR, [0x56] = K_KP_MINUS,
	[0x57] = K_KP_PLUS, [0x58] = K_KP_ENTER,
	[0x59] = K_KP_END, [0x5A] = K_KP_DOWNARROW, [0x5B] = K_KP_PGDN, [0x5C] = K_KP_LEFTARROW,
	[0x5D] = K_KP_5, [0x5E] = K_KP_RIGHTARROW, [0x5F] = K_KP_HOME, [0x60] = K_KP_UPARROW,
	[0x61] = K_KP_PGUP, [0x62] = K_KP_INS, [0x63] = K_KP_DEL,
	[0x65] = K_MENU, [0x67] = K_KP_EQUALS,
	[0x68] = K_F13, [0x69] = K_F14, [0x6A] = K_F15,
	[0xE0] = K_CTRL, [0xE1] = K_SHIFT, [0xE2] = K_ALT, [0xE3] = K_SUPER,
	[0xE4] = K_CTRL, [0xE5] = K_SHIFT, [0xE6] = K_ALT, [0xE7] = K_SUPER,
};

// The mouse buttons in the order of N_BUTTON_* (common/input_abi.h): bit i is button i + 1.
static const int buttonKeys[] = { K_MOUSE1, K_MOUSE2, K_MOUSE3, K_MOUSE4, K_MOUSE5 };


/*
===============
IN_IsConsoleKey

A character listed in cl_consoleKeys toggles the console; the key itself is the one at the grave accent.
===============
*/
static qboolean IN_IsConsoleKey( keyNum_t key, int character )
{
	typedef struct consoleKey_s
	{
		enum
		{
			QUAKE_KEY,
			CHARACTER
		} type;

		union
		{
			keyNum_t key;
			int character;
		} u;
	} consoleKey_t;

	static consoleKey_t consoleKeys[ MAX_CONSOLE_KEYS ];
	static int numConsoleKeys = 0;
	int i;

	// Only parse the variable when it changes
	if ( cl_consoleKeys->modified )
	{
		const char *text_p, *token;

		cl_consoleKeys->modified = qfalse;
		text_p = cl_consoleKeys->string;
		numConsoleKeys = 0;

		while ( numConsoleKeys < MAX_CONSOLE_KEYS )
		{
			consoleKey_t *c = &consoleKeys[ numConsoleKeys ];
			int charCode = 0;

			token = COM_Parse( &text_p );
			if ( !token[ 0 ] )
				break;

			charCode = Com_HexStrToInt( token );

			if ( charCode > 0 )
			{
				c->type = CHARACTER;
				c->u.character = charCode;
			}
			else
			{
				c->type = QUAKE_KEY;
				c->u.key = Key_StringToKeynum( token );

				// 0 isn't a key
				if ( c->u.key <= 0 )
					continue;
			}

			numConsoleKeys++;
		}
	}

	// If the character is the same as the key, prefer the character
	if ( key == character )
		key = 0;

	for ( i = 0; i < numConsoleKeys; i++ )
	{
		consoleKey_t *c = &consoleKeys[ i ];

		switch ( c->type )
		{
			case QUAKE_KEY:
				if ( key && c->u.key == key )
					return qtrue;
				break;

			case CHARACTER:
				if ( c->u.character == character )
					return qtrue;
				break;
		}
	}

	return qfalse;
}


static int IN_UsageToKey( uint32_t usage )
{
	if ( ( usage >> 16 ) != N_PAGE_KEYBOARD )
		return 0;
	usage &= 0xffff;
	return usage < ARRAY_LEN( usageKeys ) ? usageKeys[ usage ] : 0;
}


static void IN_SyncModifiers( uint32_t mods )
{
	lastMods = mods;
	keys[ K_CTRL ].down = ( mods & NMOD_CTRL ) ? qtrue : qfalse;
	keys[ K_SHIFT ].down = ( mods & NMOD_SHIFT ) ? qtrue : qfalse;
	keys[ K_ALT ].down = ( mods & NMOD_ALT ) ? qtrue : qfalse;
	keys[ K_CAPSLOCK ].down = ( mods & NMOD_CAPS ) ? qtrue : qfalse;
}


/*
===================
Key_CapsLockOn
===================
*/
qboolean Key_CapsLockOn( void )
{
	return ( lastMods & NMOD_CAPS ) ? qtrue : qfalse;
}


static void IN_QueueKey( int key, qboolean down )
{
	Com_QueueEvent( in_eventTime, SE_KEY, key, down, 0, NULL );
}


static void IN_KeyEvent( const struct gui_event *e )
{
	qboolean down = e->pressed ? qtrue : qfalse;
	int key = IN_UsageToKey( e->usage );

	if ( e->repeat && Key_GetCatcher() == 0 )
		return;

	if ( key == K_ENTER && down && keys[ K_ALT ].down ) {
		Cvar_SetIntegerValue( "r_fullscreen", nwin.isFullscreen ? 0 : 1 );
		Cbuf_AddText( "vid_restart\n" );
		return;
	}

	if ( key ) {
		IN_QueueKey( key, down );
	}

	if ( !down )
		return;

	if ( key == K_BACKSPACE ) {
		Com_QueueEvent( in_eventTime, SE_CHAR, CTRL( 'h' ), 0, 0, NULL );
	} else if ( key == K_ESCAPE ) {
		Com_QueueEvent( in_eventTime, SE_CHAR, key, 0, 0, NULL );
	} else if ( keys[ K_CTRL ].down && key >= 'a' && key <= 'z' ) {
		Com_QueueEvent( in_eventTime, SE_CHAR, CTRL( key ), 0, 0, NULL );
	} else if ( key != K_CONSOLE && N_KEY_IS_TEXT( e->key ) && !keys[ K_CTRL ].down ) {
		if ( IN_IsConsoleKey( 0, (int)e->key ) ) {
			IN_QueueKey( K_CONSOLE, qtrue );
			IN_QueueKey( K_CONSOLE, qfalse );
		} else {
			Com_QueueEvent( in_eventTime, SE_CHAR, (int)e->key, 0, 0, NULL );
		}
	}
}


static void IN_ButtonsEvent( uint32_t buttons )
{
	uint32_t changed = buttons ^ heldButtons;
	int bit;

	for ( bit = 0; bit < 32; bit++ ) {
		int key;

		if ( !( changed & ( 1u << bit ) ) )
			continue;

		key = bit < (int)ARRAY_LEN( buttonKeys ) ? buttonKeys[ bit ] : K_AUX1 + ( bit - (int)ARRAY_LEN( buttonKeys ) ) % 16;
		IN_QueueKey( key, ( buttons & ( 1u << bit ) ) ? qtrue : qfalse );
	}

	heldButtons = buttons;
}


static void IN_ReleaseAll( void )
{
	if ( heldButtons )
		IN_ButtonsEvent( 0 );
	wheelRemainder = 0;
	Key_ClearStates();
}


static void IN_WheelEvent( int amount )
{
	// a positive amount turns the wheel toward the user: the content moves down
	wheelRemainder += amount;

	while ( wheelRemainder >= N_WHEEL_DETENT ) {
		IN_QueueKey( K_MWHEELDOWN, qtrue );
		IN_QueueKey( K_MWHEELDOWN, qfalse );
		wheelRemainder -= N_WHEEL_DETENT;
	}
	while ( wheelRemainder <= -N_WHEEL_DETENT ) {
		IN_QueueKey( K_MWHEELUP, qtrue );
		IN_QueueKey( K_MWHEELUP, qfalse );
		wheelRemainder += N_WHEEL_DETENT;
	}
}


/*
===============
Nocturne_WindowEvent
===============
*/
void Nocturne_WindowEvent( const struct gui_event *e )
{
	in_eventTime = Sys_Milliseconds();

	switch ( e->type ) {
	case EV_KEY:
		IN_SyncModifiers( e->mods );
		IN_KeyEvent( e );
		break;

	case EV_MOUSE_MOVE:
		if ( pointerLocked && ( e->dx || e->dy ) )
			Com_QueueEvent( in_eventTime, SE_MOUSE, e->dx, e->dy, 0, NULL );
		break;

	case EV_MOUSE_DOWN:
	case EV_MOUSE_UP:
		IN_SyncModifiers( e->mods );
		IN_ButtonsEvent( e->buttons );
		break;

	case EV_WHEEL:
		IN_WheelEvent( e->wheel_y );
		break;

	case EV_RESIZE:
		nwin.width = e->x;
		nwin.height = e->y;
		break;

	case EV_FOCUS:
		IN_ReleaseAll();
		gw_active = qtrue;
		gw_minimized = qfalse;
		break;

	case EV_UNFOCUS:
		IN_ReleaseAll();
		gw_active = qfalse;
		pointerLocked = qfalse;
		pointerLockWanted = qfalse;
		break;

	case EV_POINTER_LOCK:
		pointerLocked = e->key ? qtrue : qfalse;
		pointerLockWanted = pointerLocked;
		break;

	case EV_INPUT_LOST:
		// the keys and buttons held before it are taken as released
		IN_ReleaseAll();
		break;

	case EV_CLOSE:
		Cbuf_ExecuteText( EXEC_NOW, "quit Closed window\n" );
		break;

	default:
		break;
	}
}


/*
===============
HandleEvents
===============
*/
void HandleEvents( void )
{
	struct gui_event e;
	n_result r;

	if ( !nwin.win )
		return;

	while ( ( r = win_event( nwin.win, &e, 0 ) ) > 0 )
		Nocturne_WindowEvent( &e );

	if ( r < 0 )
		Cbuf_ExecuteText( EXEC_NOW, "quit Closed window\n" );
}


static void IN_SetPointerLock( qboolean wanted )
{
	if ( wanted == pointerLockWanted || !nwin.win )
		return;

	// the lock is taken by the foreground window with focus; EV_POINTER_LOCK says when it holds
	if ( win_pointer_lock( nwin.win, wanted ? true : false ) >= 0 )
		pointerLockWanted = wanted;
}


/*
===============
IN_Frame
===============
*/
void IN_Frame( void )
{
	qboolean wanted = gw_active && !gw_minimized && !in_nograb->integer;

	// the console of a window is used with the system's pointer
	if ( ( Key_GetCatcher() & KEYCATCH_CONSOLE ) && !nwin.isFullscreen )
		wanted = qfalse;

	if ( in_mouse->integer == 0 )
		wanted = qfalse;

	IN_SetPointerLock( wanted );
}


static void IN_Restart( void )
{
	IN_Shutdown();
	IN_Init();
}


/*
===============
IN_Init
===============
*/
void IN_Init( void )
{
	Com_DPrintf( "\n------- Input Initialization -------\n" );

	in_nograb = Cvar_Get( "in_nograb", "0", CVAR_ARCHIVE );
	Cvar_SetDescription( in_nograb, "Do not capture mouse in game, may be useful during online streaming." );

	in_mouse = Cvar_Get( "in_mouse", "1", CVAR_ARCHIVE );
	Cvar_CheckRange( in_mouse, "0", "1", CV_INTEGER );
	Cvar_SetDescription( in_mouse, "Mouse data input source:\n 0 - disable mouse input\n 1 - raw mouse input, the window holds the pointer" );

	// ~ and `, as keys and characters
	cl_consoleKeys = Cvar_Get( "cl_consoleKeys", "~ ` 0x7e 0x60", CVAR_ARCHIVE );
	Cvar_SetDescription( cl_consoleKeys, "Space delimited list of key names or characters that toggle the console." );

	Cmd_AddCommand( "in_restart", IN_Restart );

	Com_DPrintf( "------------------------------------\n" );
}


/*
===============
IN_Shutdown
===============
*/
void IN_Shutdown( void )
{
	if ( nwin.win && pointerLockWanted )
		win_pointer_lock( nwin.win, false );

	pointerLocked = qfalse;
	pointerLockWanted = qfalse;
	heldButtons = 0;

	Cmd_RemoveCommand( "in_restart" );
}
