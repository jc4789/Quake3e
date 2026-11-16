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
// UDP の socket は一つで、両方の family（IPv4、IPv6）の全部の住所に立つ（common/net_abi.h）。
// 住所が取る port は、netadr_t の約束どおり網の順（BigShort）で持ち、endpoint の port は機械の順。

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "../client/client.h"

#include <nocturne.h>
#include <nocturne_services.h>
#include "nocturne_local.h"

#define MAX_LOCAL_ADDRESSES 64
#define MAX_RESOLVED 16

// LAN の探しで NA_MULTICAST6 が指す群（IPv6、範囲は admin-local）。
#define MULTICAST6_GROUP "ff04::696f:7175:616b:6533"

static cvar_t *net_enabled;
static cvar_t *net_port;
static cvar_t *net_dropsim;

static n_handle udpSocket = -1;
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


/*
===================
Address conversion
===================
*/
static void EndpointToNetadr( const struct n_net_endpoint *ep, netadr_t *a )
{
	Com_Memset( a, 0, sizeof( *a ) );

	if ( ep->ip.family == N_NET_IPV4 ) {
		a->type = NA_IP;
		Com_Memcpy( a->ipv._4, ep->ip.bytes, 4 );
	} else if ( ep->ip.family == N_NET_IPV6 ) {
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
		ep->ip = n_net_ip_v4( a->ipv._4[0], a->ipv._4[1], a->ipv._4[2], a->ipv._4[3] );
		break;
	case NA_IP6:
		ep->ip.family = N_NET_IPV6;
		Com_Memcpy( ep->ip.bytes, a->ipv._6, 16 );
		ep->ip.zone = a->scope_id;
		break;
	case NA_MULTICAST6:
		if ( !n_net_ip_parse( MULTICAST6_GROUP, &ep->ip ) )
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

Does not parse port numbers, only base addresses.
=============
*/
qboolean Sys_StringToAdr( const char *s, netadr_t *a, netadrtype_t family )
{
	struct n_net_ip found[MAX_RESOLVED];
	struct n_dns_result result;
	struct n_net_endpoint ep;
	uint32_t families = 0;
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
	if ( n_net_ip_parse( s, &ep.ip ) ) {
		found[0] = ep.ip;
		r = 1;
	} else {
		r = net_resolve( s, families, found, MAX_RESOLVED, &result, NULL, -1 );
		if ( r < 0 ) {
			Com_Printf( "%s: Error resolving %s\n", __func__, s );
			return qfalse;
		}
		r = result.count < MAX_RESOLVED ? result.count : MAX_RESOLVED;
	}

	// IPv4 is preferred unless net_enabled asks for IPv6 first (bit 4)
	pick = -1;
	for ( i = 0; i < r; i++ ) {
		qboolean v6 = found[i].family == N_NET_IPV6;
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
		Com_Printf( "%s: Error resolving %s: No address of required type found.\n", __func__, s );
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
		if ( n_net_ip_format( &ep.ip, s, sizeof( s ) ) == 0 )
			s[0] = '\0';
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
Local addresses
===================
*/
static int LocalAddresses( struct n_net_address *out, int max )
{
	n_result count = n_net_addresses( out, (size_t)max );

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

LAN clients will have their rate var ignored
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
		// RFC1918 and the loopback network
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
	char text[N_NET_IP_TEXT_BYTES];
	int i, count;

	count = LocalAddresses( local, MAX_LOCAL_ADDRESSES );
	for ( i = 0; i < count; i++ ) {
		if ( n_net_ip_format( &local[i].ip, text, sizeof( text ) ) == 0 )
			continue;
		if ( local[i].ip.family == N_NET_IPV4 )
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
	n_result r = udp_send_to( udpSocket, to, data, (size_t)length );

	// a full send queue drops the packet, as a full socket buffer did
	if ( r < 0 && r != -N_ERR_NOT_READY && !quiet )
		Com_Printf( "Sys_SendPacket: error %i\n", (int)-r );
}


// A limited broadcast needs the network capability; a program sends to each network's directed broadcast.
static void SendBroadcast( const struct n_net_endpoint *to, int length, const void *data )
{
	struct n_net_address local[MAX_LOCAL_ADDRESSES];
	int i, count;

	count = LocalAddresses( local, MAX_LOCAL_ADDRESSES );
	for ( i = 0; i < count; i++ ) {
		struct n_net_endpoint target = *to;
		int bits = local[i].prefix, byteIndex;

		if ( local[i].ip.family != N_NET_IPV4 || local[i].state != N_NET_PREFERRED || bits >= 32 )
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

	if ( udpSocket < 0 )
		return;

	if ( to->type == NA_MULTICAST6 && ( net_enabled->integer & NET_DISABLEMCAST ) )
		return;

	if ( to->type == NA_BROADCAST ) {
		Com_Memset( &ep, 0, sizeof( ep ) );
		ep.ip.family = N_NET_IPV4;
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

Delivers every datagram that waits on the socket.
==================
*/
static void NET_Event( void )
{
	byte bufData[ MAX_MSGLEN_BUF ];
	netadr_t from;
	msg_t netmsg;

	while ( 1 ) {
		struct n_net_received info;
		n_result r;

		MSG_Init( &netmsg, bufData, MAX_MSGLEN );
		Com_Memset( &info, 0, sizeof( info ) );

		r = udp_receive_from( udpSocket, netmsg.data, netmsg.maxsize, &info );
		if ( r == -N_ERR_NOT_READY || r == -N_ERR_CLOSED )
			break;
		if ( r < 0 )
			continue; // an error notice for a datagram sent; the socket stays usable

		if ( r >= netmsg.maxsize ) {
			EndpointToNetadr( &info.from, &from );
			Com_Printf( "Oversize packet from %s\n", NET_AdrToString( &from ) );
			continue;
		}

		EndpointToNetadr( &info.from, &from );
		if ( from.type == NA_BAD )
			continue;

		netmsg.cursize = (int)r;
		netmsg.readcount = 0;

		if ( net_dropsim->value > 0.0f && net_dropsim->value <= 100.0f ) {
			// net_dropsim percent of incoming packets get dropped
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

Sleeps msec or until something happens on the network

Returns qfalse on network event or qtrue in all other cases
====================
*/
qboolean NET_Sleep( int timeout )
{
	uint64_t deadline;

	if ( timeout < 0 )
		timeout = 0;

	deadline = n_clock_after( (uint64_t)timeout * N_NS_PER_US );

	if ( udpSocket < 0 ) {
		n_sleep_until( deadline );
		return qtrue;
	}

	if ( n_net_wait( udpSocket, N_EVENT_RECEIVABLE, deadline, -1 ) > 0 ) {
		NET_Event();
		return qfalse;
	}

	return qtrue;
}


/*
====================
IPv6 multicast

The scan for servers on the local network sends to MULTICAST6_GROUP; a server joins it on every adapter.
====================
*/
static void MulticastMembership( qboolean enter )
{
	struct n_net_address local[MAX_LOCAL_ADDRESSES];
	struct n_net_ip group;
	int i, count;

	if ( !n_net_ip_parse( MULTICAST6_GROUP, &group ) )
		return;

	count = LocalAddresses( local, MAX_LOCAL_ADDRESSES );
	for ( i = 0; i < count; i++ ) {
		if ( local[i].ip.family == N_NET_IPV6 && n_net_ip_is_link_local( &local[i].ip ) )
			udp_join( udpSocket, &group, local[i].adapter, enter );
	}
}


void NET_JoinMulticast6( void )
{
	if ( udpSocket < 0 || multicastJoined || ( net_enabled->integer & NET_DISABLEMCAST ) )
		return;

	MulticastMembership( qtrue );
	multicastJoined = qtrue;
}


void NET_LeaveMulticast6( void )
{
	if ( udpSocket < 0 || !multicastJoined )
		return;

	MulticastMembership( qfalse );
	multicastJoined = qfalse;
}


/*
====================
NET_OpenSocket

Scans for a free port, so several servers can start without a different net_port each.
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
		local.ip.family = families == NET_ENABLEV4 ? N_NET_IPV4 : families == NET_ENABLEV6 ? N_NET_IPV6 : N_NET_ANY;
		local.port = (uint16_t)( port + i );

		udpSocket = udp_open( &local );
		if ( udpSocket >= 0 ) {
			Cvar_SetIntegerValue( "net_port", port + i );
			return;
		}
		udpSocket = -1;
	}

	Com_Printf( "WARNING: Couldn't open a UDP socket.\n" );
}


static void NET_CloseSocket( void )
{
	if ( udpSocket >= 0 ) {
		multicastJoined = qfalse;
		n_handle_close( udpSocket );
		udpSocket = -1;
	}
}


static qboolean NET_GetCvars( void )
{
	int modified;

	// A server's owner turns IPv6 on; a client has it so it can reach IPv6-only hosts.
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
