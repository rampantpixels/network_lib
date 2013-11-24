/* tcp.c  -  Network library  -  Internal use only  -  2013 Mattias Jansson / Rampant Pixels
 * 
 * This library provides a network abstraction built on foundation streams.
 *
 * All rights reserved. No part of this library, code or built products may be used without
 * the explicit consent from Rampant Pixels AB
 * 
 */

#include <network/tcp.h>
#include <network/address.h>
#include <network/event.h>
#include <network/internal.h>

#include <foundation/foundation.h>

#if FOUNDATION_PLATFORM_POSIX
#  include <netinet/tcp.h>
#endif

static socket_t* _tcp_socket_allocate( void );
static void _tcp_socket_open( socket_t*, unsigned int );
static int  _tcp_socket_connect( socket_t*, const network_address_t*, unsigned int );
static void _tcp_socket_set_delay( socket_t*, bool );
static void _tcp_socket_buffer_read( socket_t*, unsigned int );
static void _tcp_socket_buffer_write( socket_t* );


static socket_t* _tcp_socket_allocate( void )
{
	socket_t* sock = _socket_allocate();
	if( !sock )
		return 0;

	sock->open_fn = _tcp_socket_open;
	sock->connect_fn = _tcp_socket_connect;
	sock->read_fn = _tcp_socket_buffer_read;
	sock->write_fn = _tcp_socket_buffer_write;
	
	return sock;
}


object_t tcp_socket_create( void )
{
	socket_t* sock = _tcp_socket_allocate();
	return sock ? sock->id : 0;
}


bool tcp_socket_listen( object_t id )
{
	socket_base_t* sockbase;
	socket_t* sock = _socket_lookup( id );

	if( !sock )
		return false;

	if( sock->base < 0 )
	{
		socket_free( id );
		return false;
	}
	
	sockbase = _socket_base + sock->base;
	if( ( sockbase->state != SOCKETSTATE_NOTCONNECTED ) ||
	    ( sockbase->fd == SOCKET_INVALID ) ||
	    !sock->address_local ) //Must be locally bound
	{
		socket_free( id );
		return false;
	}

	if( listen( sockbase->fd, SOMAXCONN ) == 0 )
	{
#if BUILD_ENABLE_LOG
		char* address_str = network_address_to_string( sock->address_local, true );
		log_infof( HASH_NETWORK, "Listening on TCP/IP socket 0x%llx (" STRING_FORMAT_POINTER " : %d) local address %s", sock->id, sock, sockbase->fd, address_str );
		string_deallocate( address_str );
#endif
		sockbase->state = SOCKETSTATE_LISTENING;
		socket_free( id );
		return true;
	}
	
#if BUILD_ENABLE_LOG
	{
		char* address_str = network_address_to_string( sock->address_local, true );
		int sockerr = NETWORK_SOCKET_ERROR;
		log_errorf( HASH_NETWORK, ERROR_SYSTEM_CALL_FAIL, "Unable to listen on TCP/IP socket 0x%llx (" STRING_FORMAT_POINTER " : %d) on local address %s: %s", id, sock, sockbase->fd, address_str, system_error_message( sockerr ) );
		string_deallocate( address_str );
	}
#endif

	socket_free( id );
	
	return false;
}


object_t tcp_socket_accept( object_t id, unsigned int timeoutms )
{
	socket_base_t* sockbase;
	socket_base_t* acceptbase;
	socket_t* sock;
	socket_t* accepted;
	network_address_t* address_remote;
	network_address_ip_t* address_ip;
	socklen_t address_len;
	int err = 0;
	int fd;
	bool blocking;

	sock = _socket_lookup( id );
	if( !sock )
		return 0;

	if( sock->base < 0 )
	{
		socket_free( id );
		return 0;
	}

	sockbase = _socket_base + sock->base;
	if( ( sockbase->state != SOCKETSTATE_LISTENING ) ||
	    ( sockbase->fd == SOCKET_INVALID ) ||
	    !sock->address_local ) //Must be locally bound
	{
		socket_free( id );
		return 0;
	}

	blocking = ( ( sockbase->flags & SOCKETFLAG_BLOCKING ) != 0 );

	if( ( timeoutms > 0 ) && blocking )
		_socket_set_blocking( sock, false );

	address_remote = network_address_clone( sock->address_local );
	address_ip = (network_address_ip_t*)address_remote;
	address_len = address_remote->address_size;

	fd = (int)accept( sockbase->fd, &address_ip->saddr, &address_len );
	if( fd < 0 )
	{
		err = NETWORK_SOCKET_ERROR;
		if( timeoutms > 0 )
		{
#if FOUNDATION_PLATFORM_WINDOWS
			if( err == WSAEWOULDBLOCK )
#else
			if( err == EAGAIN )
#endif
			{
				struct timeval tval;
				struct fd_set fdread, fderr;
				int ret;

				FD_ZERO( &fdread );
				FD_ZERO( &fderr );
				FD_SET( (SOCKET)sockbase->fd, &fdread );
				FD_SET( (SOCKET)sockbase->fd, &fderr );

				tval.tv_sec  = timeoutms / 1000;
				tval.tv_usec = ( timeoutms % 1000 ) * 1000;

				ret = select( sockbase->fd + 1, &fdread, 0, &fderr, &tval );
				if( ret > 0 )
				{
					address_len = address_remote->address_size;
					fd = (int)accept( sockbase->fd, &address_ip->saddr, &address_len );
				}
			}
		}
	}
	
	if( ( timeoutms > 0 ) && blocking )
		_socket_set_blocking( sock, true );

	sockbase->flags &= SOCKETFLAG_CONNECTION_PENDING;

	if( fd < 0 )
	{
		memory_deallocate( address_remote );
		socket_free( id );
		return 0;
	}

	accepted = _tcp_socket_allocate();
	if( !accepted )
	{
		socket_free( id );		
		return 0;
	}

	if( _socket_allocate_base( accepted ) < 0 )
	{
		socket_free( accepted->id );
		socket_free( id );
		return 0;
	}

	acceptbase = _socket_base + accepted->base;
	acceptbase->fd = fd;
	acceptbase->state = SOCKETSTATE_CONNECTED;
	accepted->address_remote = (network_address_t*)address_remote;
	
	_socket_store_address_local( accepted, address_ip->family );

#if BUILD_ENABLE_LOG
	{
		char* address_local_str = network_address_to_string( sock->address_local, true );
		char* address_remote_str = network_address_to_string( accepted->address_remote, true );
		log_infof( HASH_NETWORK, "Accepted connection on TCP/IP socket 0x%llx (" STRING_FORMAT_POINTER " : %d) with local address %s: created socket 0x%llx (" STRING_FORMAT_POINTER " : %d) remote address %s", sock->id, sock, sockbase->fd, address_local_str, accepted->id, accepted, acceptbase->fd, address_remote_str );
		string_deallocate( address_remote_str );
		string_deallocate( address_local_str );
	}
#endif

	socket_free( id );
	
	return accepted->id;
}


bool tcp_socket_delay( object_t id )
{
	bool delay = false;
	socket_t* sock = _socket_lookup( id );
	if( sock && ( sock->base >= 0 ) )
	{
		socket_base_t* sockbase = _socket_base + sock->base;
		delay = ( ( sockbase->flags & SOCKETFLAG_TCPDELAY ) != 0 );
		socket_free( id );
	}
	return delay;
}


void tcp_socket_set_delay( object_t id, bool delay )
{
	socket_t* sock = _socket_lookup( id );
	if( !sock )
	{
		log_errorf( HASH_NETWORK, ERROR_INVALID_VALUE, "Trying to set delay flag on an invalid socket 0x%llx", id );
		return;
	}

	_tcp_socket_set_delay( sock, delay );
	
	socket_free( id );
}


static void _tcp_socket_open( socket_t* sock, unsigned int family )
{
	socket_base_t* sockbase;

	if( sock->base < 0 )
		return;
	
	sockbase = _socket_base + sock->base;
	if( family == NETWORK_ADDRESSFAMILY_IPV6 )
		sockbase->fd = (int)socket( AF_INET6, SOCK_STREAM, IPPROTO_TCP );
	else
		sockbase->fd = (int)socket( AF_INET, SOCK_STREAM, IPPROTO_TCP );

	if( sockbase->fd < 0 )
	{
		log_errorf( HASH_NETWORK, ERROR_SYSTEM_CALL_FAIL, "Unable to open TCP/IP socket 0x%llx (" STRING_FORMAT_POINTER " : %d): %s", sock->id, sock, sockbase->fd, system_error_message( NETWORK_SOCKET_ERROR ) );
		sockbase->fd = SOCKET_INVALID;
	}
	else
	{
		log_debugf( HASH_NETWORK, "Opened TCP/IP socket 0x%llx (" STRING_FORMAT_POINTER " : %d)", sock->id, sock, sockbase->fd );

		_socket_set_blocking( sock, sockbase->flags & SOCKETFLAG_BLOCKING );
		_tcp_socket_set_delay( sock, sockbase->flags & SOCKETFLAG_TCPDELAY );
	}
}


static int _tcp_socket_connect( socket_t* sock, const network_address_t* address, unsigned int timeoutms )
{
	socket_base_t* sockbase;
	const network_address_ip_t* address_ip;
	bool blocking;
	bool failed = true;
	int err = 0;
	const char* error_message = 0;

	if( sock->base < 0 )
		return 0;

	sockbase = _socket_base + sock->base;
	blocking = ( ( sockbase->flags & SOCKETFLAG_BLOCKING ) != 0 );

	if( ( timeoutms > 0 ) && blocking )
		_socket_set_blocking( sock, false );

	address_ip = (const network_address_ip_t*)address;
	err = connect( sockbase->fd, &address_ip->saddr, address_ip->address_size );
	if( !err )
	{
		failed = false;
		sockbase->state = SOCKETSTATE_CONNECTED;
	}
	else
	{
		bool in_progress = false;
		err = NETWORK_SOCKET_ERROR;
#if FOUNDATION_PLATFORM_WINDOWS
		in_progress = ( err == WSAEWOULDBLOCK );
#else //elif FOUDATION_PLATFORM_POSIX
		in_progress = ( err == EINPROGRESS );
#endif
		if( in_progress )
		{
			if( !timeoutms )
			{
				failed = false;
				sockbase->state = SOCKETSTATE_CONNECTING;
			}
			else
			{
				struct timeval tv;
				fd_set fdwrite, fderr;
				int ret;

				FD_ZERO( &fdwrite );
				FD_ZERO( &fderr );
				FD_SET( (SOCKET)sockbase->fd, &fdwrite );
				FD_SET( (SOCKET)sockbase->fd, &fderr );
				
				tv.tv_sec  = timeoutms / 1000;
				tv.tv_usec = ( timeoutms % 1000 ) * 1000;

				ret = select( (int)( sockbase->fd + 1 ), 0, &fdwrite, &fderr, &tv );
				if( ret > 0 )
				{
#if FOUNDATION_PLATFORM_WINDOWS
					int serr = 0;
					int slen = sizeof( int );
					getsockopt( sockbase->fd, SOL_SOCKET, SO_ERROR, (char*)&serr, &slen );
#else
					int serr = 0;
					socklen_t slen = sizeof( int );
					getsockopt( sockbase->fd, SOL_SOCKET, SO_ERROR, (void*)&serr, &slen );
#endif
					if( !serr )
					{
						failed = false;
						sockbase->state = SOCKETSTATE_CONNECTED;
					}
					else
					{
						err = serr;
						error_message = "select indicated socket error";
					}
				}
				else if( ret < 0 )
				{
					err = NETWORK_SOCKET_ERROR;
					error_message = "select failed";
				}
				else
				{
#if FOUNDATION_PLATFORM_WINDOWS
					err = WSAETIMEDOUT;
#else
					err = ETIMEDOUT;
#endif
					error_message = "select timed out";
				}			
			}
		}
		else
		{
			error_message = "connect error";
		}
	}
	
	if( ( timeoutms > 0 ) && blocking )
		_socket_set_blocking( sock, true );

	if( failed )
		return err;

	memory_deallocate( sock->address_remote );
	sock->address_remote = network_address_clone( address );

	if( !sock->address_local )
		_socket_store_address_local( sock, address_ip->family );

#if BUILD_ENABLE_DEBUG_LOG
	{
		char* address_str = network_address_to_string( address, true );
		log_debugf( HASH_NETWORK, "%s socket 0x%llx (" STRING_FORMAT_POINTER " : %d) to remote host %s", ( sockbase->state == SOCKETSTATE_CONNECTING ) ? "Connecting" : "Connected", sock->id, sock, sockbase->fd, address_str );
		string_deallocate( address_str );
	}
#endif
	
	return 0;
}


static void _tcp_socket_set_delay( socket_t* sock, bool delay )
{
	socket_base_t* sockbase;
	int flag;
	FOUNDATION_ASSERT( sock );
	if( sock->base < 0 )
		return;
	sockbase = _socket_base + sock->base;
	sockbase->flags = ( delay ? sockbase->flags | SOCKETFLAG_TCPDELAY : sockbase->flags & ~SOCKETFLAG_TCPDELAY );
	flag = ( delay ? 0 : 1 );
	if( sockbase->fd != SOCKET_INVALID )
		setsockopt( sockbase->fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof( int ) );
}


static void _tcp_socket_buffer_read( socket_t* sock, unsigned int wanted_size )
{
	socket_base_t* sockbase;
	unsigned int available;
	unsigned int max_read = 0;
	unsigned int try_read;
	long ret;

	if( sock->base < 0 )
		return;
	
	if( sock->offset_write_in >= sock->offset_read_in )
	{
		max_read = BUILD_SIZE_SOCKET_READBUFFER - sock->offset_write_in;
		if( !sock->offset_read_in )
			--max_read; //Don't read if write ptr wraps to 0 and equals read ptr (then the entire buffer is discarded)
	}
	else
		max_read = sock->offset_read_in - sock->offset_write_in - 1; //-1 so write ptr doesn't end up equal to read ptr (then the entire buffer is discarded)

	if( !max_read )
		return;

	try_read = max_read;
	if( wanted_size && ( try_read > wanted_size ) )
		try_read = wanted_size;

	sockbase = _socket_base + sock->base;
	available = _socket_available_fd( sockbase->fd );
	if( !available && !wanted_size && ( sockbase->flags & SOCKETFLAG_BLOCKING ) )
		return;

	if( available > try_read )
		try_read = ( max_read < available ) ? max_read : available;

	ret = recv( sockbase->fd, (char*)sock->buffer_in + sock->offset_write_in, try_read, 0 );
	if( !ret )
	{
#if BUILD_ENABLE_DEBUG_LOG
		char* address_str = network_address_to_string( sock->address_remote, true );
		log_debugf( HASH_NETWORK, "Socket closed gracefully on remote end 0x%llx (" STRING_FORMAT_POINTER " : %d): %s", sock->id, sock, sockbase->fd, address_str );
		string_deallocate( address_str );
#endif
		_socket_close( sock );
		if( !( sockbase->flags & SOCKETFLAG_HANGUP_PENDING ) )
		{
			sockbase->flags |= SOCKETFLAG_HANGUP_PENDING;
			network_event_post( NETWORKEVENT_HANGUP, sock->id );
		}
		return;
	}
	else if( ret > 0 )
	{
#if BUILD_ENABLE_NETWORK_DUMP_TRAFFIC > 0
#if BUILD_ENABLE_NETWORK_DUMP_TRAFFIC > 1
		const unsigned char* src = (const unsigned char*)sock->buffer_in + sock->offset_write_in;
		char dump_buffer[66];
#endif
		log_debugf( HASH_NETWORK, "Socket 0x%llx (" STRING_FORMAT_POINTER " : %d) read %d of %u (%u were available) bytes from TCP/IP socket to buffer position %d", sock->id, sock, sockbase->fd, ret, try_read, available, sock->offset_write_in );
#if BUILD_ENABLE_NETWORK_DUMP_TRAFFIC > 1
		for( long row = 0; row <= ( ret / 8 ); ++row )
		{
			long ofs = 0, col = 0, cols = 8;
			if( ( row + 1 ) * 8 > ret )
				cols = ret - ( row * 8 );
			for( ; col < cols; ++col, ofs +=3 )
			{
				string_format_buffer( dump_buffer + ofs, 66 - ofs, "%02x", *( src + ( row * 8 ) + col ) );
				*( dump_buffer + ofs + 2 ) = ' ';
			}
			if( ofs )
			{
				*( dump_buffer + ofs - 1 ) = 0;
				log_context_debug( HASH_NETWORK, dump_buffer );
			}
		}
#endif
#endif

		sock->offset_write_in += ret;
		FOUNDATION_ASSERT_MSG( sock->offset_write_in <= BUILD_SIZE_SOCKET_READBUFFER, "Buffer overwrite" );
		if( sock->offset_write_in >= BUILD_SIZE_SOCKET_READBUFFER )
			sock->offset_write_in = 0;
	}
	else
	{
		int sockerr = NETWORK_SOCKET_ERROR;
#if FOUNDATION_PLATFORM_WINDOWS
		if( sockerr != WSAEWOULDBLOCK )
#else
		if( sockerr != EAGAIN )
#endif
		{
			log_warnf( HASH_NETWORK, WARNING_SYSTEM_CALL_FAIL, "Socket recv() failed on 0x%llx (" STRING_FORMAT_POINTER " : %d): %s (%d)", sock->id, sock, sockbase->fd, system_error_message( sockerr ), sockerr );
		}

#if FOUNDATION_PLATFORM_WINDOWS
		if( ( sockerr == WSAENETDOWN ) || ( sockerr == WSAENETRESET ) || ( sockerr == WSAENOTCONN ) || ( sockerr == WSAECONNABORTED ) || ( sockerr == WSAECONNRESET ) || ( sockerr == WSAETIMEDOUT ) )
#else
		if( ( sockerr == ECONNRESET ) || ( sockerr == EPIPE ) || ( sockerr == ETIMEDOUT ) )
#endif
		{
			_socket_close( sock );
			if( !( sockbase->flags & SOCKETFLAG_HANGUP_PENDING ) )
			{
				sockbase->flags |= SOCKETFLAG_HANGUP_PENDING;
				network_event_post( NETWORKEVENT_HANGUP, sock->id );
			}
		}

		_socket_poll_state( sockbase );
	}

	//If we were at end of read buffer, try more data if wanted
	if( ( sockbase->state == SOCKETSTATE_CONNECTED ) && ( try_read < wanted_size ) && ( available > try_read ) && ( sock->offset_write_in == 0 ) && ( sock->offset_read_in > 1 ) && ( ret > 0 ) )
		_tcp_socket_buffer_read( sock, wanted_size - try_read );	
}


static void _tcp_socket_buffer_write( socket_t* sock )
{
	socket_base_t* sockbase;
	unsigned int sent = 0;
	if( sock->base < 0 )
		return;
	
	sockbase = _socket_base + sock->base;
	while( sent < sock->offset_write_out )
	{
		long res = send( sockbase->fd, (const char*)sock->buffer_out + sent, sock->offset_write_out - sent, 0 );
		if( res > 0 )
		{
#if BUILD_ENABLE_NETWORK_DUMP_TRAFFIC > 0
#if BUILD_ENABLE_NETWORK_DUMP_TRAFFIC > 1
			const unsigned char* src = (const unsigned char*)sock->buffer_out + sent;
			char buffer[34];
#endif
			log_context_debugf( HASH_NETWORK, "Socket 0x%llx (" STRING_FORMAT_POINTER " : %d) wrote %d of %d bytes bytes to TCP/IP socket from buffer position %d", sock->id, sock, sockbase->fd, res, sock->offset_write_out - sent, sent );
#if BUILD_ENABLE_NETWORK_DUMP_TRAFFIC > 1
			for( long row = 0; row <= ( res / 8 ); ++row )
			{
				long ofs = 0, col = 0, cols = 8;
				if( ( row + 1 ) * 8 > res )
					cols = res - ( row * 8 );
				for( ; col < cols; ++col, ofs +=3 )
				{
					string_format_buffer( buffer + ofs, 34 - ofs, "%02x", *( src + ( row * 8 ) + col ) );
					*( buffer + ofs + 2 ) = ' ';
				}
				if( ofs )
				{
					*( buffer + ofs - 1 ) = 0;
					log_context_debug( HASH_NETWORK, buffer );
				}
			}
#endif
#endif
			sent += res;
		}
		else
		{
			if( res < 0 )
			{
				int sockerr = NETWORK_SOCKET_ERROR;

#if FOUNDATION_PLATFORM_WINDOWS
				int serr = 0;
				int slen = sizeof( int );
				getsockopt( sockbase->fd, SOL_SOCKET, SO_ERROR, (char*)&serr, &slen );
#else
				int serr = 0;
				socklen_t slen = sizeof( int );
				getsockopt( sockbase->fd, SOL_SOCKET, SO_ERROR, (void*)&serr, &slen );
#endif

#if FOUNDATION_PLATFORM_WINDOWS
				if( sockerr == WSAEWOULDBLOCK )
#else
				if( sockerr == EAGAIN )
#endif
				{
					log_warnf( HASH_NETWORK, WARNING_SUSPICIOUS, "Partial TCP socket send() on 0x%llx (" STRING_FORMAT_POINTER " : %d): %d of %d bytes written to socket (SO_ERROR %d)", sock->id, sock, sockbase->fd, sent, sock->offset_write_out, serr );
					sockbase->flags |= SOCKETFLAG_REFLUSH;
				}
				else
				{
					log_warnf( HASH_NETWORK, WARNING_SYSTEM_CALL_FAIL, "Socket send() failed on 0x%llx (" STRING_FORMAT_POINTER " : %d): %s (%d) (SO_ERROR %d)", sock->id, sock, sockbase->fd, system_error_message( sockerr ), sockerr, serr );
				}

#if FOUNDATION_PLATFORM_WINDOWS
				if( ( sockerr == WSAENETDOWN ) || ( sockerr == WSAENETRESET ) || ( sockerr == WSAENOTCONN ) || ( sockerr == WSAECONNABORTED ) || ( sockerr == WSAECONNRESET ) || ( sockerr == WSAETIMEDOUT ) )
#else
				if( ( sockerr == ECONNRESET ) || ( sockerr == EPIPE ) || ( sockerr == ETIMEDOUT ) )
#endif
				{
					_socket_close( sock );
					if( !( sockbase->flags & SOCKETFLAG_HANGUP_PENDING ) )
					{
						sockbase->flags |= SOCKETFLAG_HANGUP_PENDING;
						network_event_post( NETWORKEVENT_HANGUP, sock->id );
					}
				}

				if( sockbase->state != SOCKETSTATE_NOTCONNECTED )
					_socket_poll_state( sockbase );
			}

			if( sent )
			{
				memmove( sock->buffer_out, sock->buffer_out + sent, sock->offset_write_out - sent );
				sock->offset_write_out -= sent;
			}

			return;
		}
	}

	sockbase->flags &= ~SOCKETFLAG_REFLUSH;
	sock->offset_write_out = 0;
}
