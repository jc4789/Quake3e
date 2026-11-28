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

#include <nocturne.h>
#include "nocturne_local.h"


qboolean Sys_Mkdir( const char *path )
{
	struct kabi_entry entry;

	if ( n_entry_info( path, &entry ) >= 0 )
		return entry.kind == KABI_KIND_FOLDER ? qtrue : qfalse;

	return n_folder_create( path ) >= 0 ? qtrue : qfalse;
}


/*
=================
Sys_FOpen

ISO fopen, except that a folder is not a file.
=================
*/
FILE *Sys_FOpen( const char *ospath, const char *mode )
{
	struct kabi_entry entry;

	if ( n_entry_info( ospath, &entry ) >= 0 && entry.kind == KABI_KIND_FOLDER )
		return NULL;

	return fopen( ospath, mode );
}


// Documents have no read-only attribute.
qboolean Sys_ResetReadOnlyAttribute( const char *ospath )
{
	(void)ospath;
	return qfalse;
}


/*
=================
Sys_Pwd

The folder of the program's document.
=================
*/
const char *Sys_Pwd( void )
{
	static char pwd[ MAX_OSPATH ];
	const char *program, *name;

	if ( pwd[0] )
		return pwd;

	program = n_program_path();
	name = n_path_name( program );

	Q_strncpyz( pwd, program, (int)MIN( (size_t)( name - program ), sizeof( pwd ) ) + 1 );
	// without the separator before the name
	if ( pwd[0] && ( pwd[ strlen( pwd ) - 1 ] == '\\' || pwd[ strlen( pwd ) - 1 ] == '/' ) )
		pwd[ strlen( pwd ) - 1 ] = '\0';

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

	if ( n_app_data_folder( homePath, sizeof( homePath ) ) < 0 )
		homePath[0] = '\0';

	return homePath;
}


// Quake 3 is not sold through a store that installs it somewhere else.
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
	struct kabi_entry entry;

	if ( n_entry_info( filename, &entry ) >= 0 && entry.kind != KABI_KIND_FOLDER ) {
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
Listing
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
	struct kabi_entry *entries;
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

	count = n_folder_entries( search, &entries );
	if ( count < 0 )
		return 0;

	for ( e = 0; e < count; e++ ) {
		const char *name = entries[e].name;
		qboolean isFolder = entries[e].kind == KABI_KIND_FOLDER ? qtrue : qfalse;

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
