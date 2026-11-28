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

// Nocturne のファイルとパス。
// fs_basepath は、このプログラムの置き場（C:\Programs\<フォルダー>）。fs_homepath は、利用者ごとのアプリのデータ
// （C:\Users\<名前>\AppData\Quake3e）。パスの区切りは "/" と "\" のどちらでもよく、名前の大小は区別されない。

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"

#include <nocturne/core.h>
#include "nocturne_local.h"


// path の項の記録を entry に読む。項が無ければ偽。
static qboolean EntryInfo( const char *path, struct n_entry *entry )
{
	memset( entry, 0, sizeof( *entry ) );
	entry->bytes = sizeof( *entry );
	return n_entry_info( path, strlen( path ), entry ) >= 0 ? qtrue : qfalse;
}


// path のフォルダーの項の全部を malloc した配列に読み、数を返す（失敗は負）。数を聞いてから読み、その間に項が
// 増えれば読み直す。
static n_result FolderList( const char *path, struct n_entry **entries )
{
	size_t length = strlen( path );
	n_result count = n_folder_list( path, length, NULL, 0, sizeof( struct n_entry ), 0 );

	*entries = NULL;
	while ( count >= 0 ) {
		size_t capacity = (size_t)count + 1, i;
		struct n_entry *list = calloc( capacity, sizeof( *list ) );
		n_result total;

		if ( !list )
			return -N_ERR_NO_MEMORY;
		for ( i = 0; i < capacity; i++ )
			list[i].bytes = sizeof( list[i] );
		total = n_folder_list( path, length, list, capacity, sizeof( *list ), 0 );
		if ( total >= 0 && (size_t)total <= capacity ) {
			*entries = list;
			return total;
		}
		free( list );
		count = total;
	}
	return count;
}


qboolean Sys_Mkdir( const char *path )
{
	struct n_entry entry;

	if ( EntryInfo( path, &entry ) )
		return entry.kind == N_KIND_FOLDER ? qtrue : qfalse;

	return n_folder_create( path, strlen( path ) ) >= 0 ? qtrue : qfalse;
}


/*
=================
Sys_FOpen

ISO の fopen。ただしフォルダーはファイルとして開かない。
=================
*/
FILE *Sys_FOpen( const char *ospath, const char *mode )
{
	struct n_entry entry;

	if ( EntryInfo( ospath, &entry ) && entry.kind == N_KIND_FOLDER )
		return NULL;

	return fopen( ospath, mode );
}


// 文書に読むだけの属性は無い。
qboolean Sys_ResetReadOnlyAttribute( const char *ospath )
{
	(void)ospath;
	return qfalse;
}


/*
=================
Sys_Pwd

このプログラムの文書が入っているフォルダー。
=================
*/
const char *Sys_Pwd( void )
{
	static char pwd[ MAX_OSPATH ];
	n_result length, name;

	if ( pwd[0] )
		return pwd;

	length = n_program_path( pwd, sizeof( pwd ) - 1 );
	if ( length < 0 || (size_t)length >= sizeof( pwd ) ) {
		pwd[0] = '\0';
		return pwd;
	}
	name = n_path_name( pwd, (size_t)length );
	if ( name < 0 )
		name = 0;
	// 名前の前の区切りは含めない
	if ( name > 0 && ( pwd[ name - 1 ] == '\\' || pwd[ name - 1 ] == '/' ) )
		name--;
	pwd[ name ] = '\0';

	return pwd;
}


const char *Sys_DefaultBasePath( void )
{
	return Sys_Pwd();
}


const char *Sys_DefaultHomePath( void )
{
	static char homePath[ MAX_OSPATH ];

	if ( homePath[0] )
		return homePath;

	n_result length = n_app_data_folder( homePath, sizeof( homePath ) - 1 );
	homePath[ length < 0 || (size_t)length >= sizeof( homePath ) ? 0 : length ] = '\0';

	return homePath;
}


// Quake 3 は、ほかの場所へ入れる店では配られない。
const char *Sys_SteamPath( void )
{
	return "";
}


/*
=================
Sys_GetFileStats
=================
*/
qboolean Sys_GetFileStats( const char *filename, fileOffset_t *size, fileTime_t *mtime, fileTime_t *ctime )
{
	struct n_entry entry;

	if ( EntryInfo( filename, &entry ) && entry.kind != N_KIND_FOLDER ) {
		*size = (fileOffset_t)entry.size;
		*mtime = *ctime = (fileTime_t)entry.modified;
		return qtrue;
	}

	*size = 0;
	*mtime = *ctime = 0;
	return qfalse;
}


/*
=================
一覧
=================
*/
static int Sys_ListExtFiles( const char *directory, const char *subdir, const char *extension, const char *filter, char **list, int maxfiles, int subdirs )
{
	char		search[MAX_OSPATH * 2 + MAX_QPATH + 1];
	char		filename[MAX_OSPATH * 2];
	int		nfiles = 0;
	int		extLen;
	qboolean	hasPatterns;
	qboolean	dironly;
	struct n_entry *entries;
	n_result	count, e;

	if ( extension[0] == '/' && extension[1] == 0 ) {
		extension = "";
		dironly = qtrue;
	} else {
		dironly = qfalse;
	}

	extLen = (int)strlen( extension );
	hasPatterns = Com_HasPatterns( extension ); // contains either '?' or '*'
	if ( hasPatterns && extension[0] == '.' && extension[1] != '\0' ) {
		extension++;
	}

	if ( *subdir != '\0' ) {
		Com_sprintf( search, sizeof( search ), "%s/%s", directory, subdir );
	} else {
		Com_sprintf( search, sizeof( search ), "%s", directory );
	}

	count = FolderList( search, &entries );
	if ( count < 0 )
		return 0;

	for ( e = 0; e < count; e++ ) {
		const char *name = entries[e].name;
		qboolean isFolder = entries[e].kind == N_KIND_FOLDER ? qtrue : qfalse;

		if ( isFolder ) {
			// handle recursion
			if ( subdirs > 0 ) {
				char subdir2[MAX_OSPATH * 2 + MAX_QPATH + 1];
				if ( *subdir != '\0' ) {
					Com_sprintf( subdir2, sizeof( subdir2 ), "%s/%s", subdir, name );
				} else {
					Q_strncpyz( subdir2, name, sizeof( subdir2 ) );
				}
				if ( nfiles >= maxfiles ) {
					break;
				}
				nfiles += Sys_ListExtFiles( directory, subdir2, extension, filter, list + nfiles, maxfiles - nfiles, subdirs - 1 );
			}
			if ( !dironly ) {
				continue;
			}
		} else {
			if ( dironly ) {
				continue;
			}
		}
		if ( *subdir != '\0' ) {
			Com_sprintf( filename, sizeof( filename ), "%s/%s", subdir, name );
		} else {
			Q_strncpyz( filename, name, sizeof( filename ) );
		}
		if ( filter != NULL && *filter != '\0' ) {
			if ( !Com_FilterPath( filter, filename ) ) {
				continue;
			}
		} else if ( *extension != '\0' ) {
			if ( hasPatterns ) {
				const char *x = strrchr( name, '.' );
				if ( x == NULL || !Com_FilterExt( extension, x + 1 ) ) {
					continue;
				}
			} else {
				// check for exact extension
				const int length = (int)strlen( name );
				if ( length < extLen || Q_stricmp( name + length - extLen, extension ) ) {
					continue;
				}
			}
		}
		if ( nfiles >= maxfiles ) {
			break;
		}
		list[ nfiles++ ] = FS_CopyString( filename );
	}

	free( entries );

	return nfiles;
}


char **Sys_ListFiles( const char *directory, const char *extension, const char *filter, int *numfiles, int subdirs )
{
	char**	listCopy;
	char*	list[MAX_FOUND_FILES];
	int	i, nfiles;

	if ( extension == NULL ) {
		extension = "";
	}

	nfiles = Sys_ListExtFiles( directory, "", extension, filter, list, ARRAY_LEN( list ), subdirs );

	// copy list from stack, reserve extra space for NULL
	listCopy = Z_Malloc( ( nfiles + 1 ) * sizeof( listCopy[0] ) );
	for ( i = 0; i < nfiles; i++ ) {
		listCopy[i] = list[i];
	}
	listCopy[i] = NULL;

	if ( nfiles > 1 ) {
		Com_SortList( listCopy, nfiles - 1 );
	}

	*numfiles = nfiles;
	return listCopy;
}


void Sys_FreeFileList( char **list )
{
	int i;

	if ( !list ) {
		return;
	}

	for ( i = 0; list[i]; i++ ) {
		Z_Free( list[i] );
	}

	Z_Free( list );
}
