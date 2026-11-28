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

// Nocturne の音：ミキサーが書く輪（dma.buffer）を、audio サービスの stream（n_audio_open）へ順に出す。
// 輪の読み位置は stream が鳴らし終えた frame の数（n_audio_status の played_frames）で、ミキサーはその先を書く。
// stream は一つの thread から使う約束なので、出す・聞く仕事は ポンプの thread だけが行い、位置は atomic で渡す。

#include <stdatomic.h>

#include "../qcommon/q_shared.h"
#include "../client/snd_local.h"
#include "../client/client.h"

#include <nocturne.h>
#include "nocturne_local.h"

#define RING_SAMPLES		65536	// mono samples in the ring: 32768 frames, 0.68 s at 48 kHz
#define KEEP_AHEAD_MS		80		// how far ahead of the sound being heard the pump keeps the stream filled
#define PUMP_SLEEP_MS		1
#define MAX_CHUNK_FRAMES	4096

static n_audio_stream *stream;
static qboolean snd_inited;
static atomic_bool pumpStop;
static atomic_uint_fast64_t playedFrames;
static int pumpThread;


static int SNDDMA_KHzToHz( int khz )
{
	switch ( khz ) {
	case 48: return 48000;
	case 44: return 44100;
	case 22: return 22050;
	case 11: return 11025;
	case 8:  return 8000;
	default: return 0;
	}
}


/*
===============
SNDDMA_Pump

Keeps the stream filled to KEEP_AHEAD_MS with what the ring holds from where the last submit ended.
Each pass either submits at least one frame, or sleeps one millisecond: the loop ends when pumpStop is set.
===============
*/
static void SNDDMA_Pump( void *unused )
{
	const int channels = dma.channels;
	const int ringFrames = dma.fullsamples;
	const size_t frameBytes = (size_t)channels * ( dma.samplebits / 8 );
	const uint64_t keepFrames = (uint64_t)dma.speed * KEEP_AHEAD_MS / 1000;
	uint64_t sent = 0;

	(void)unused;

	while ( !atomic_load( &pumpStop ) ) {
		struct kabi_audio_status status;
		uint64_t ahead;

		if ( n_audio_status( stream, &status ) < 0 ) {
			n_sleep_until( n_clock_after( PUMP_SLEEP_MS * KABI_NS_PER_MS ) );
			continue;
		}

		atomic_store( &playedFrames, status.played_frames );

		ahead = (uint64_t)status.queued_frames + status.device_pending_frames;
		if ( ahead < keepFrames ) {
			size_t at = (size_t)( sent % (uint64_t)ringFrames );
			size_t count = (size_t)( keepFrames - ahead );
			n_result taken;

			if ( count > MAX_CHUNK_FRAMES )
				count = MAX_CHUNK_FRAMES;
			if ( count > (size_t)ringFrames - at )
				count = (size_t)ringFrames - at;

			taken = n_audio_submit( stream, dma.buffer + at * frameBytes, count, 0 );
			if ( taken > 0 ) {
				sent += (uint64_t)taken;
				continue;
			}
		}

		n_sleep_until( n_clock_after( PUMP_SLEEP_MS * KABI_NS_PER_MS ) );
	}

}


/*
===============
SNDDMA_Init
===============
*/
qboolean SNDDMA_Init( void )
{
	struct kabi_audio_format format;
	n_result r;

	if ( snd_inited )
		return qtrue;

	dma.speed = SNDDMA_KHzToHz( s_khz->integer );
	if ( dma.speed == 0 )
		dma.speed = 22050;

	dma.channels = 2;
	dma.samplebits = 16;
	dma.isfloat = 0;
	dma.samples = RING_SAMPLES;
	dma.fullsamples = dma.samples / dma.channels;
	dma.submission_chunk = 1;
	dma.driver = "Nocturne audio";

	format.rate = (uint32_t)dma.speed;
	format.channels = (uint16_t)dma.channels;
	format.sample = KABI_AUDIO_S16;

	r = n_audio_open( &format, &stream );
	if ( r < 0 ) {
		Com_Printf( "n_audio_open failed: %i\n", (int)-r );
		stream = NULL;
		return qfalse;
	}

	dma.buffer = calloc( 1, (size_t)dma.samples * ( dma.samplebits / 8 ) );
	if ( !dma.buffer ) {
		n_audio_close( stream );
		stream = NULL;
		return qfalse;
	}

	atomic_store( &playedFrames, 0 );
	atomic_store( &pumpStop, false );

	pumpThread = (int)thread_create( SNDDMA_Pump, NULL );
	if ( pumpThread < 0 ) {
		Com_Printf( "could not start the sound thread: %i\n", pumpThread );
		free( dma.buffer );
		dma.buffer = NULL;
		n_audio_close( stream );
		stream = NULL;
		return qfalse;
	}

	Com_Printf( "Nocturne audio: %i Hz, %i channels, 16 bit, ring of %i frames\n", dma.speed, dma.channels, dma.fullsamples );

	snd_inited = qtrue;
	return qtrue;
}


/*
===============
SNDDMA_GetDMAPos

The ring position of the sound being heard, in mono samples.
===============
*/
int SNDDMA_GetDMAPos( void )
{
	uint64_t frames = atomic_load( &playedFrames );

	return (int)( ( frames % (uint64_t)dma.fullsamples ) * (uint64_t)dma.channels );
}


/*
===============
SNDDMA_Shutdown
===============
*/
void SNDDMA_Shutdown( void )
{
	if ( !snd_inited )
		return;

	atomic_store( &pumpStop, true );
	thread_join( pumpThread );

	// close lets what is queued play out
	n_audio_flush( stream );
	n_audio_close( stream );
	stream = NULL;

	free( dma.buffer );
	dma.buffer = NULL;

	snd_inited = qfalse;
}


// The mixer writes the ring while the pump reads it behind the heard position, as a sound card's DMA reads
// the ring; neither side holds a lock.
void SNDDMA_BeginPainting( void )
{
}


void SNDDMA_Submit( void )
{
}
