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

// Nocturne の起動、終了、エラー、時刻、眠り。

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "../client/client.h"

#include <nocturne.h>
#include "nocturne_local.h"

// Time since the first call, from the monotonic clock.
static uint64_t timeBase;

static uint64_t ElapsedNs( void )
{
	uint64_t now = n_clock_monotonic();

	if ( !timeBase )
		timeBase = now;

	return now - timeBase;
}


int Sys_Milliseconds( void )
{
	return (int)( ElapsedNs() / N_NS_PER_MS );
}


int64_t Sys_Microseconds( void )
{
	return (int64_t)( ElapsedNs() / N_NS_PER_US );
}


/*
==================
Sys_Sleep

Block execution for msec; a negative msec waits for a network packet.
==================
*/
void Sys_Sleep( int msec )
{
	if ( msec < 0 ) {
		// reached only by a dedicated server with no map loaded
		while ( NET_Sleep( 3000 * 1000 ) )
			;
		return;
	}

	if ( msec == 0 )
		return;

	n_sleep_until( n_clock_after( (uint64_t)msec * N_NS_PER_MS ) );
}


void Sys_SendKeyEvents( void )
{
	HandleEvents();
}


qboolean Sys_RandomBytes( byte *string, int len )
{
	return n_random( string, (size_t)len ) >= 0 ? qtrue : qfalse;
}


// The floor of the machines Nocturne runs on is far above the 64 MB this asks about.
qboolean Sys_LowPhysicalMemory( void )
{
	return qfalse;
}


/*
==================
Sys_LoadLibrary

Nocturne のライブラリー（.NLIB）は、呼ぶ側が使う契約（プロバイダー provider の major、要る minor）を名指して中核の
ライブラリーの実装の読み込み（n_image_load）で読み、シンボルはそのプロバイダーのエクスポートから引く。読み込んだ
イメージはプロセスの終わりまで外れないので、同じ文書をまた読み込めば同じイメージが返り、Sys_UnloadLibrary は何も
しない。文書からの読み込みなので、実行できる記憶を新しく作る権利の無いプロセスでは読み込めない。
==================
*/
typedef struct nocturneLibrary_s {
	struct nocturneLibrary_s *next;
	const struct n_image	*image;
	char					provider[];		// 引くプロバイダー
} nocturneLibrary_t;

// 読み込んだライブラリーの記録。イメージと同じくプロセスの終わりまで残り、同じイメージとプロバイダーには同じ記録を返す。
static nocturneLibrary_t *libraries;
static int loadFunctionErrors;

void *Sys_LoadLibrary( const char *name, const char *provider, int major, int minor )
{
	const struct n_image *image;
	struct n_link_failure failure;
	nocturneLibrary_t *library;
	n_handle document;
	n_result r;
	size_t bytes;

	document = n_document_open( name, N_DOCUMENT_READ );
	if ( document < 0 ) {
		// 探す道（FS_LoadLibrary）の中の無い文書は失敗として伝えない
		if ( document != -N_ERR_NOT_FOUND )
			Com_Printf( "%s を開けない：%s\n", name, n_result_text( document ) );
		return NULL;
	}
	r = n_image_load( document, -1, provider, (uint32_t)major, (uint32_t)minor, &image );
	n_handle_close( document );
	if ( r < 0 ) {
		if ( r == -N_ERR_WRONG_GENERATION )
			Com_Printf( "%s は契約 %s %d.%d を提供しない\n", name, provider, major, minor );
		else if ( n_image_failure( &failure ) >= 0 )
			Com_Printf( "%s を読み込めない：%s（%s の要求 %s %u.%u%s%s）\n", name, n_result_text( r ), failure.image,
				failure.provider, failure.major, failure.minor, failure.symbol[0] ? " の " : "", failure.symbol );
		else
			Com_Printf( "%s を読み込めない：%s\n", name, n_result_text( r ) );
		return NULL;
	}

	for ( library = libraries; library; library = library->next ) {
		if ( library->image == image && !strcmp( library->provider, provider ) )
			return library;
	}
	bytes = strlen( provider ) + 1;
	library = malloc( sizeof( *library ) + bytes );
	if ( !library )
		Com_Error( ERR_FATAL, "Sys_LoadLibrary: 記憶が足りない" );
	library->image = image;
	memcpy( library->provider, provider, bytes );
	library->next = libraries;
	libraries = library;
	return library;
}


void *Sys_LoadFunction( void *handle, const char *name )
{
	const nocturneLibrary_t *library = handle;
	void *symbol = n_image_symbol( library->image, library->provider, name );

	if ( !symbol ) {
		Com_Printf( "Sys_LoadFunction: プロバイダー %s のエクスポート %s が無い\n", library->provider, name );
		loadFunctionErrors++;
	}
	return symbol;
}


int Sys_LoadFunctionErrors( void )
{
	int result = loadFunctionErrors;

	loadFunctionErrors = 0;
	return result;
}


// 何もしない：イメージはプロセスの終わりまで外れない。
void Sys_UnloadLibrary( void *handle )
{
	(void)handle;
}


void Sys_Init( void )
{
	Cvar_Set( "arch", OS_STRING " " ARCH_STRING );
}


// single exit point
void NORETURN Sys_Exit( int code )
{
	exit( code );
}


void NORETURN Sys_Quit( void )
{
	CL_Shutdown( "", qtrue );

	Sys_Exit( 0 );
}


void NORETURN FORMAT_PRINTF(1, 2) QDECL Sys_Error( const char *format, ... )
{
	va_list argptr;
	char text[1024];

	va_start( argptr, format );
	Q_vsnprintf( text, sizeof( text ), format, argptr );
	va_end( argptr );

	CL_Shutdown( text, qtrue );

	fprintf( stderr, "Sys_Error: %s\n", text );

	Sys_Exit( 1 );
}


/*
==================
Sys_Print

The log of the program (stderr), without the control characters of the color codes.
==================
*/
void Sys_Print( const char *msg )
{
	char printmsg[ MAXPRINTMSG ];
	char *out = printmsg;

	while ( *msg != '\0' && out < printmsg + sizeof( printmsg ) ) {
		if ( ( *msg >= ' ' && *msg != 127 ) || *msg == '\n' || *msg == '\t' )
			*out++ = *msg;
		msg++;
	}

	fwrite( printmsg, 1, (size_t)( out - printmsg ), stderr );
}


int main( int argc, char **argv )
{
	char con_title[ MAX_CVAR_VALUE_STRING ];
	int xpos, ypos;
	char *cmdline;
	size_t len;
	int i;

	// merge the command line, this is kinda silly
	for ( len = 1, i = 1; i < argc; i++ )
		len += strlen( argv[i] ) + 1;

	cmdline = malloc( len );
	*cmdline = '\0';
	for ( i = 1; i < argc; i++ ) {
		if ( i > 1 )
			strcat( cmdline, " " );
		strcat( cmdline, argv[i] );
	}

	Com_EarlyParseCmdLine( cmdline, con_title, sizeof( con_title ), &xpos, &ypos );

	Com_Init( cmdline );

	while ( 1 ) {
		IN_Frame();
		Com_Frame( CL_NoDelay() );
	}

	// never gets here
	return 0;
}
