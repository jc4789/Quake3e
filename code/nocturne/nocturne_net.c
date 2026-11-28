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

// Nocturne の網：qcommon/net_ip.c の持つ口を、Nocturne の UDP と名前引きの上に置く。
// UDP の socket は一つで、両方の family（IPv4、IPv6）の全部の住所に立つ（nocturne.core の net の udp_open）。
// 住所が取る port は、netadr_t の約束どおり網の順（BigShort）で持ち、endpoint の port は機械の順。

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "../client/client.h"

#include <nocturne/core.h>
#include "nocturne_local.h"

#define MAX_LOCAL_ADDRESSES 64
#define MAX_RESOLVED 16

// LAN の探しで NA_MULTICAST6 が指す群（IPv6、範囲は admin-local）。
#define MULTICAST6_GROUP "ff04::696f:7175:616b:6533"

static cvar_t *net_enabled;
static cvar_t *net_port;
static cvar_t *net_dropsim;

static n_udp udpSocket = { -1 };
static qboolean networkingEnabled = qfalse;
static qboolean multicastJoined = qfalse;

static uint16_t PortToHost( uint16_t port )
{
	return (uint16_t)BigShort( (short)port );
}

static uint16_t PortToNet( uint16_t port )
{
	return (uint16_t)BigShort( (short)port );
}


// NUL で終わる text が住所の文字なら *ip に読んで真。
static qboolean ParseIp( const char *text, struct n_net_ip *ip )
{
	return n_net_ip_parse( text, strlen( text ), ip ) >= 0 ? qtrue : qfalse;
}


// ip の文字を text に NUL で終えて書く。書けなければ偽で、text は空。
static qboolean FormatIp( const struct n_net_ip *ip, char *text, size_t capacity )
{
	n_result n = n_net_ip_format( ip, text, capacity - 1 );

	if ( n < 0 || (size_t)n >= capacity ) {
		text[0] = '\0';
		return qfalse;
	}
	text[n] = '\0';
	return qtrue;
}


/*
===================
住所の変換
===================
*/
static void EndpointToNetadr( const struct n_net_endpoint *ep, netadr_t *a )
{
	Com_Memset( a, 0, sizeof( *a ) );

	if ( ep->ip.family == N_NET_FAMILY_IPV4 ) {
		a->type = NA_IP;
		Com_Memcpy( a->ipv._4, ep->ip.bytes, 4 );
	} else if ( ep->ip.family == N_NET_FAMILY_IPV6 ) {
		a->type = NA_IP6;
		Com_Memcpy( a->ipv._6, ep->ip.bytes, 16 );
		a->scope_id = ep->ip.zone;
	} else {
		a->type = NA_BAD;
		return;
	}
	a->port = PortToNet( ep->port );
}


static qboolean NetadrToEndpoint( const netadr_t *a, struct n_net_endpoint *ep )
{
	Com_Memset( ep, 0, sizeof( *ep ) );

	switch ( a->type ) {
	case NA_IP:
		ep->ip.family = N_NET_FAMILY_IPV4;
		Com_Memcpy( ep->ip.bytes, a->ipv._4, 4 );
		break;
	case NA_IP6:
		ep->ip.family = N_NET_FAMILY_IPV6;
		Com_Memcpy( ep->ip.bytes, a->ipv._6, 16 );
		ep->ip.zone = a->scope_id;
		break;
	case NA_MULTICAST6:
		if ( !ParseIp( MULTICAST6_GROUP, &ep->ip ) )
			return qfalse;
		break;
	default:
		return qfalse;
	}
	ep->port = PortToHost( a->port );
	return qtrue;
}


/*
=============
Sys_StringToAdr

port の番号は読まず、住所だけを読む。
=============
*/
qboolean Sys_StringToAdr( const char *s, netadr_t *a, netadrtype_t family )
{
	struct n_net_ip found[MAX_RESOLVED];
	struct n_dns_result result = { .bytes = sizeof( result ) };
	struct n_net_endpoint ep;
	n_dns_family families = 0;
	n_result r;
	int i, pick;

	if ( family == NA_IP )
		families = N_DNS_FAMILY_IPV4;
	else if ( family == NA_IP6 )
		families = N_DNS_FAMILY_IPV6;
	else {
		if ( !( net_enabled->integer & NET_ENABLEV4 ) )
			families = N_DNS_FAMILY_IPV6;
		else if ( !( net_enabled->integer & NET_ENABLEV6 ) )
			families = N_DNS_FAMILY_IPV4;
	}

	Com_Memset( &ep, 0, sizeof( ep ) );
	if ( ParseIp( s, &ep.ip ) ) {
		found[0] = ep.ip;
		r = 1;
	} else {
		r = n_net_resolve( s, strlen( s ), families, NULL, N_CLOCK_NEVER, NULL, 0, found, MAX_RESOLVED, &result );
		if ( r < 0 ) {
			Com_Printf( "%s: %s の名前を引けない\n", __func__, s );
			return qfalse;
		}
		if ( r > MAX_RESOLVED )
			r = MAX_RESOLVED;
	}

	// net_enabled が IPv6 を先にする（bit 4）のでなければ IPv4 を選ぶ
	pick = -1;
	for ( i = 0; i < r; i++ ) {
		qboolean v6 = found[i].family == N_NET_FAMILY_IPV6;
		if ( v6 && !( net_enabled->integer & NET_ENABLEV6 ) )
			continue;
		if ( !v6 && !( net_enabled->integer & NET_ENABLEV4 ) )
			continue;
		if ( pick < 0 || ( ( net_enabled->integer & 4 ) != 0 ) == v6 ) {
			pick = i;
			if ( ( ( net_enabled->integer & 4 ) != 0 ) == v6 )
				break;
		}
	}

	if ( pick < 0 ) {
		Com_Printf( "%s: %s の名前を引けない：求める種類の住所が無い\n", __func__, s );
		return qfalse;
	}

	ep.ip = found[pick];
	ep.port = 0;
	EndpointToNetadr( &ep, a );
	return qtrue;
}


/*
===================
NET_CompareBaseAdrMask

Compare without port, and up to the bit number given in netmask.
===================
*/
qboolean NET_CompareBaseAdrMask( const netadr_t *a, const netadr_t *b, unsigned int netmask )
{
	byte cmpmask, *addra, *addrb;
	int curbyte;

	if ( a->type != b->type )
		return qfalse;

	if ( a->type == NA_LOOPBACK )
		return qtrue;

	if ( a->type == NA_IP ) {
		addra = (byte *) &a->ipv._4;
		addrb = (byte *) &b->ipv._4;

		if ( netmask > 32 )
			netmask = 32;
	}
	else if ( a->type == NA_IP6 ) {
		addra = (byte *) &a->ipv._6;
		addrb = (byte *) &b->ipv._6;

		if ( netmask > 128 )
			netmask = 128;
	}
	else {
		Com_Printf( "%s: bad address type\n", __func__ );
		return qfalse;
	}

	curbyte = netmask >> 3;

	if ( curbyte && memcmp( addra, addrb, curbyte ) )
		return qfalse;

	netmask &= 0x07;
	if ( netmask ) {
		cmpmask = (1 << netmask) - 1;
		cmpmask <<= 8 - netmask;

		if ( ( addra[curbyte] & cmpmask ) == ( addrb[curbyte] & cmpmask ) )
			return qtrue;
	}
	else
		return qtrue;

	return qfalse;
}


qboolean NET_CompareBaseAdr( const netadr_t *a, const netadr_t *b )
{
	return NET_CompareBaseAdrMask( a, b, ~0U );
}


const char *NET_AdrToString( const netadr_t *a )
{
	static char s[NET_ADDRSTRMAXLEN];
	struct n_net_endpoint ep;

	if ( a->type == NA_LOOPBACK )
		strcpy( s, "loopback" );
	else if ( a->type == NA_BOT )
		strcpy( s, "bot" );
	else if ( NetadrToEndpoint( a, &ep ) ) {
		FormatIp( &ep.ip, s, sizeof( s ) );
	}
	else
		s[0] = '\0';

	return s;
}


const char *NET_AdrToStringwPort( const netadr_t *a )
{
	static char s[NET_ADDRSTRMAXLEN + 8]; // including strlen("[]:65535")

	if ( a->type == NA_LOOPBACK )
		strcpy( s, "loopback" );
	else if ( a->type == NA_BOT )
		strcpy( s, "bot" );
	else if ( a->type == NA_IP )
		Com_sprintf( s, sizeof( s ), "%s:%hu", NET_AdrToString( a ), PortToHost( a->port ) );
	else if ( a->type == NA_IP6 )
		Com_sprintf( s, sizeof( s ), "[%s]:%hu", NET_AdrToString( a ), PortToHost( a->port ) );
	else
		s[0] = '\0';

	return s;
}


qboolean NET_CompareAdr( const netadr_t *a, const netadr_t *b )
{
	if ( !NET_CompareBaseAdr( a, b ) )
		return qfalse;

	if ( a->type == NA_IP || a->type == NA_IP6 ) {
		if ( a->port == b->port )
			return qtrue;
	}
	else
		return qtrue;

	return qfalse;
}


qboolean NET_IsLocalAddress( const netadr_t *adr )
{
	return adr->type == NA_LOOPBACK;
}


/*
===================
この機械の住所
===================
*/
static int LocalAddresses( struct n_net_address *out, int max )
{
	n_result count;
	int i;

	for ( i = 0; i < max; i++ ) {
		Com_Memset( &out[i], 0, sizeof( out[i] ) );
		out[i].bytes = sizeof( out[i] );
	}
	count = n_net_addresses( out, (size_t)max, sizeof( out[0] ) );
	if ( count < 0 )
		return 0;
	return count < max ? (int)count : max;
}


static qboolean SamePrefix( const struct n_net_ip *a, const struct n_net_ip *b, int bits )
{
	int whole = bits >> 3;
	int rest = bits & 7;

	if ( a->family != b->family )
		return qfalse;
	if ( memcmp( a->bytes, b->bytes, whole ) != 0 )
		return qfalse;
	if ( rest ) {
		byte mask = (byte)( 0xff << ( 8 - rest ) );
		if ( ( a->bytes[whole] & mask ) != ( b->bytes[whole] & mask ) )
			return qfalse;
	}
	return qtrue;
}


/*
==================
Sys_IsLANAddress

LAN のクライアントには rate の変数を使わない。
==================
*/
qboolean Sys_IsLANAddress( const netadr_t *adr )
{
	struct n_net_address local[MAX_LOCAL_ADDRESSES];
	struct n_net_endpoint ep;
	int i, count;

	if ( adr->type == NA_LOOPBACK )
		return qtrue;

	if ( adr->type == NA_IP ) {
		// RFC 1918 の網と loopback の網
		if ( adr->ipv._4[0] == 10 )
			return qtrue;
		if ( adr->ipv._4[0] == 172 && ( adr->ipv._4[1] & 0xf0 ) == 16 )
			return qtrue;
		if ( adr->ipv._4[0] == 192 && adr->ipv._4[1] == 168 )
			return qtrue;
		if ( adr->ipv._4[0] == 127 )
			return qtrue;
	}
	else if ( adr->type == NA_IP6 ) {
		if ( adr->ipv._6[0] == 0xfe && ( adr->ipv._6[1] & 0xc0 ) == 0x80 )
			return qtrue;
		if ( ( adr->ipv._6[0] & 0xfe ) == 0xfc )
			return qtrue;
	}

	if ( !NetadrToEndpoint( adr, &ep ) )
		return qfalse;

	count = LocalAddresses( local, MAX_LOCAL_ADDRESSES );
	for ( i = 0; i < count; i++ ) {
		if ( SamePrefix( &local[i].ip, &ep.ip, local[i].prefix ) )
			return qtrue;
	}

	return qfalse;
}


/*
==================
Sys_ShowIP
==================
*/
void Sys_ShowIP( void )
{
	struct n_net_address local[MAX_LOCAL_ADDRESSES];
	char text[N_NET_IP_TEXT_MAX + 1];
	int i, count;

	count = LocalAddresses( local, MAX_LOCAL_ADDRESSES );
	for ( i = 0; i < count; i++ ) {
		if ( !FormatIp( &local[i].ip, text, sizeof( text ) ) )
			continue;
		if ( local[i].ip.family == N_NET_FAMILY_IPV4 )
			Com_Printf( "IP: %s\n", text );
		else
			Com_Printf( "IP6: %s\n", text );
	}
}


/*
==================
Sys_SendPacket
==================
*/
static void SendTo( const struct n_net_endpoint *to, int length, const void *data, qboolean quiet )
{
	n_result r = n_udp_send( udpSocket, to, data, (size_t)length );

	// 送りの量が上限ならパケットを捨てる
	if ( r < 0 && r != -N_ERR_NOT_READY && !quiet )
		Com_Printf( "Sys_SendPacket: 誤り %i\n", (int)-r );
}


// 網の全体への broadcast はネットワークのケイパビリティが要るので、プログラムは網ごとの directed broadcast へ送る。
static void SendBroadcast( const struct n_net_endpoint *to, int length, const void *data )
{
	struct n_net_address local[MAX_LOCAL_ADDRESSES];
	int i, count;

	count = LocalAddresses( local, MAX_LOCAL_ADDRESSES );
	for ( i = 0; i < count; i++ ) {
		struct n_net_endpoint target = *to;
		int bits = local[i].prefix, byteIndex;

		if ( local[i].ip.family != N_NET_FAMILY_IPV4 || local[i].state != N_NET_ADDRESS_STATE_PREFERRED || bits >= 32 )
			continue;

		target.ip = local[i].ip;
		for ( byteIndex = 0; byteIndex < 4; byteIndex++ ) {
			int hostBits = 8 * ( byteIndex + 1 ) - bits;
			if ( hostBits <= 0 )
				continue;
			target.ip.bytes[byteIndex] |= hostBits >= 8 ? 0xff : (byte)( 0xff >> ( 8 - hostBits ) );
		}
		SendTo( &target, length, data, qtrue );
	}
}


void Sys_SendPacket( int length, const void *data, const netadr_t *to )
{
	struct n_net_endpoint ep;

	switch ( to->type ) {
	case NA_BROADCAST:
	case NA_IP:
	case NA_IP6:
	case NA_MULTICAST6:
		break;
	default:
		Com_Error( ERR_FATAL, "Sys_SendPacket: bad address type %i", to->type );
		return;
	}

	if ( udpSocket.handle < 0 )
		return;

	if ( to->type == NA_MULTICAST6 && ( net_enabled->integer & NET_DISABLEMCAST ) )
		return;

	if ( to->type == NA_BROADCAST ) {
		Com_Memset( &ep, 0, sizeof( ep ) );
		ep.ip.family = N_NET_FAMILY_IPV4;
		ep.port = PortToHost( to->port );
		SendBroadcast( &ep, length, data );
		return;
	}

	if ( !NetadrToEndpoint( to, &ep ) )
		return;

	SendTo( &ep, length, data, qfalse );
}


/*
==================
NET_Event

socket に着いて待っている datagram を全部渡す。
==================
*/
static void NET_Event( void )
{
	byte bufData[ MAX_MSGLEN_BUF ];
	netadr_t from;
	msg_t netmsg;

	while ( 1 ) {
		struct n_udp_received info;
		n_result r;

		MSG_Init( &netmsg, bufData, MAX_MSGLEN );
		Com_Memset( &info, 0, sizeof( info ) );
		info.bytes = sizeof( info );

		r = n_udp_receive( udpSocket, netmsg.data, (size_t)netmsg.maxsize, &info );
		if ( r == -N_ERR_NOT_READY || r == -N_ERR_CLOSED )
			break;
		if ( r < 0 )
			continue; // 送った datagram への誤りの知らせ。socket はそのまま使える

		if ( r >= netmsg.maxsize ) {
			EndpointToNetadr( &info.from, &from );
			Com_Printf( "%s から大きすぎるパケット\n", NET_AdrToString( &from ) );
			continue;
		}

		EndpointToNetadr( &info.from, &from );
		if ( from.type == NA_BAD )
			continue;

		netmsg.cursize = (int)r;
		netmsg.readcount = 0;

		if ( net_dropsim->value > 0.0f && net_dropsim->value <= 100.0f ) {
			// 着いたパケットの net_dropsim パーセントを捨てる
			if ( rand() < (int) ( ( (double) RAND_MAX ) / 100.0 * (double) net_dropsim->value ) )
				continue;
		}

		if ( com_sv_running->integer || com_dedicated->integer )
			Com_RunAndTimeServerPacket( &from, &netmsg );
		else
			CL_PacketEvent( &from, &netmsg );
	}
}


/*
====================
NET_Sleep

timeout マイクロ秒か、網に何かが着くまで止まる。

網に着けば qfalse、ほかは qtrue を返す。
====================
*/
qboolean NET_Sleep( int timeout )
{
	uint64_t deadline;

	if ( timeout < 0 )
		timeout = 0;

	deadline = n_clock_after( (uint64_t)timeout * 1000 );

	if ( udpSocket.handle < 0 ) {
		n_sleep_until( deadline );
		return qtrue;
	}

	if ( n_net_wait( (n_handle){ udpSocket.handle }, N_EVENT_RECEIVABLE, deadline, NULL, 0 ) > 0 ) {
		NET_Event();
		return qfalse;
	}

	return qtrue;
}


/*
====================
IPv6 multicast

LAN のサーバーの探しは MULTICAST6_GROUP へ送り、サーバーは全部の adapter でその群に入る。
====================
*/
static void MulticastMembership( qboolean enter )
{
	struct n_net_address local[MAX_LOCAL_ADDRESSES];
	struct n_net_ip group;
	int i, count;

	if ( !ParseIp( MULTICAST6_GROUP, &group ) )
		return;

	count = LocalAddresses( local, MAX_LOCAL_ADDRESSES );
	for ( i = 0; i < count; i++ ) {
		if ( local[i].ip.family == N_NET_FAMILY_IPV6 && n_net_ip_is_link_local( &local[i].ip ) > 0 )
			n_udp_join( udpSocket, &group, local[i].adapter, enter ? 1 : 0 );
	}
}


void NET_JoinMulticast6( void )
{
	if ( udpSocket.handle < 0 || multicastJoined || ( net_enabled->integer & NET_DISABLEMCAST ) )
		return;

	MulticastMembership( qtrue );
	multicastJoined = qtrue;
}


void NET_LeaveMulticast6( void )
{
	if ( udpSocket.handle < 0 || !multicastJoined )
		return;

	MulticastMembership( qfalse );
	multicastJoined = qfalse;
}


/*
====================
NET_OpenSocket

空いた port を探すので、幾つものサーバーをそれぞれ別の net_port なしで起動できる。
====================
*/
static void NET_OpenSocket( void )
{
	int i, port = net_port->integer;
	int families = net_enabled->integer & ( NET_ENABLEV4 | NET_ENABLEV6 );

	if ( !families )
		return;

	for ( i = 0; i < 10; i++ ) {
		struct n_net_endpoint local;

		Com_Memset( &local, 0, sizeof( local ) );
		local.ip.family = families == NET_ENABLEV4 ? N_NET_FAMILY_IPV4 : families == NET_ENABLEV6 ? N_NET_FAMILY_IPV6 : N_NET_FAMILY_ANY;
		local.port = (uint16_t)( port + i );

		if ( n_udp_open( &local, &udpSocket ) >= 0 ) {
			Cvar_SetIntegerValue( "net_port", port + i );
			return;
		}
		udpSocket.handle = -1;
	}

	Com_Printf( "警告：UDP の socket を開けない\n" );
}


static void NET_CloseSocket( void )
{
	if ( udpSocket.handle >= 0 ) {
		multicastJoined = qfalse;
		n_handle_close( (n_handle){ udpSocket.handle } );
		udpSocket.handle = -1;
	}
}


static qboolean NET_GetCvars( void )
{
	int modified;

	// サーバーの IPv6 は所有者が入れる。クライアントは IPv6 だけのホストに届くよう入れておく。
	net_enabled = Cvar_Get( "net_enabled", com_dedicated && com_dedicated->integer ? "1" : "3",
		CVAR_LATCH | CVAR_ARCHIVE_ND | CVAR_NORESTART );
	Cvar_SetDescription( net_enabled, "Networking options, bitmask:\n"
		" 1 - enable IPv4\n"
		" 2 - enable IPv6\n"
		" 4 - prioritize IPv6 connections over IPv4\n"
		" 8 - disable IPv6 multicast" );
	Cvar_CheckRange( net_enabled, NULL, NULL, CV_INTEGER );
	modified = net_enabled->modified;
	net_enabled->modified = qfalse;

	net_port = Cvar_Get( "net_port", va( "%i", PORT_SERVER ), CVAR_LATCH | CVAR_NORESTART );
	Cvar_CheckRange( net_port, "0", "65535", CV_INTEGER );
	Cvar_SetDescription( net_port, "The network port to use." );
	modified += net_port->modified;
	net_port->modified = qfalse;

	net_dropsim = Cvar_Get( "net_dropsim", "", CVAR_TEMP );
	Cvar_SetDescription( net_dropsim, "Simulated packet drops." );

	return modified ? qtrue : qfalse;
}


static void NET_Config( qboolean enableNetworking )
{
	qboolean modified;

	NET_FlushPacketQueue( -99999 );

	// get any latched changes to cvars
	modified = NET_GetCvars();

	if ( !net_enabled->integer )
		enableNetworking = qfalse;

	// if enable state is the same and no cvars were modified, we have nothing to do
	if ( enableNetworking == networkingEnabled && !modified )
		return;

	NET_CloseSocket();
	networkingEnabled = enableNetworking;

	if ( enableNetworking )
		NET_OpenSocket();
}


static void NET_Restart_f( void )
{
	NET_Config( qtrue );
}


void NET_Init( void )
{
	NET_Config( qtrue );

	Cmd_AddCommand( "net_restart", NET_Restart_f );
}


void NET_Shutdown( void )
{
	if ( !networkingEnabled )
		return;

	NET_Config( qfalse );
}
