/* main.c  -  Network library socket test  -  Internal use only  -  2013 Mattias Jansson / Rampant Pixels
 * 
 * This library provides a network abstraction built on foundation streams.
 *
 * All rights reserved. No part of this library, code or built products may be used without
 * the explicit consent from Rampant Pixels AB
 * 
 */

#include <network/network.h>

#include <foundation/foundation.h>
#include <test/test.h>


application_t test_socket_application( void )
{
	application_t app = {0};
	app.name = "Network socket tests";
	app.short_name = "test_socket";
	app.config_dir = "test_socket";
	app.flags = APPLICATION_UTILITY;
	return app;
}


memory_system_t test_socket_memory_system( void )
{
	return memory_system_malloc();
}


int test_socket_initialize( void )
{
	return network_initialize( 1024 );
}


void test_socket_shutdown( void )
{
	network_shutdown();
}


DECLARE_TEST( tcp, create )
{
	object_t sock = tcp_socket_create();

	EXPECT_TRUE( socket_is_socket( sock ) );
	
	socket_free( sock );

	EXPECT_FALSE( socket_is_socket( sock ) );
	
	return 0;
}


DECLARE_TEST( tcp, blocking )
{
	object_t sock = tcp_socket_create();
	
	EXPECT_TRUE( socket_is_socket( sock ) );

	socket_set_blocking( sock, false );
	EXPECT_FALSE( socket_blocking( sock ) );
	
	socket_set_blocking( sock, true );
	EXPECT_TRUE( socket_blocking( sock ) );
	
	socket_free( sock );

	EXPECT_FALSE( socket_is_socket( sock ) );
	
	return 0;
}


DECLARE_TEST( tcp, bind )
{
	bool was_bound = false;
	int port;

	object_t sock = tcp_socket_create();
	
	EXPECT_TRUE( socket_is_socket( sock ) );

	EXPECT_EQ( socket_address_local( sock ), 0 );
	EXPECT_EQ( socket_address_remote( sock ), 0 );
	EXPECT_EQ( socket_state( sock ), SOCKETSTATE_NOTCONNECTED );

	for( port = 31890; !was_bound && ( port < 32890 ); ++port )	
	{
		network_address_t* address = network_address_ipv4_any();
		network_address_ip_set_port( address, port );
		
		if( socket_bind( sock, address ) )
		{
			EXPECT_NE( socket_address_local( sock ), 0 );
			EXPECT_EQ( socket_address_remote( sock ), 0 );
			EXPECT_EQ( socket_state( sock ), SOCKETSTATE_NOTCONNECTED );

			EXPECT_TRUE( network_address_equal( socket_address_local( sock ), address ) );

			was_bound = true;
		}

		memory_deallocate( address );
	}
	EXPECT_TRUE( was_bound );

	socket_free( sock );

	EXPECT_FALSE( socket_is_socket( sock ) );

	sock = tcp_socket_create();
	
	EXPECT_TRUE( socket_is_socket( sock ) );
	
	was_bound = false;
	for( port = 31890; !was_bound && ( port < 32890 ); ++port )	
	{
		network_address_t* address = network_address_ipv6_any();
		network_address_ip_set_port( address, port );
		
		if( socket_bind( sock, address ) )
		{
			EXPECT_NE( socket_address_local( sock ), 0 );
			EXPECT_EQ( socket_address_remote( sock ), 0 );
			EXPECT_EQ( socket_state( sock ), SOCKETSTATE_NOTCONNECTED );

			EXPECT_TRUE( network_address_equal( socket_address_local( sock ), address ) );

			was_bound = true;
		}

		memory_deallocate( address );
	}
	EXPECT_TRUE( was_bound );
	
	socket_free( sock );

	EXPECT_FALSE( socket_is_socket( sock ) );
	
	return 0;
}


void test_socket_declare( void )
{
	ADD_TEST( tcp, create );
	ADD_TEST( tcp, blocking );
	ADD_TEST( tcp, bind );
}


test_suite_t test_socket_suite = {
	test_socket_application,
	test_socket_memory_system,
	test_socket_declare,
	test_socket_initialize,
	test_socket_shutdown
};


#if FOUNDATION_PLATFORM_ANDROID

int test_socket_run( void )
{
	test_suite = test_socket_suite;
	return test_run_all();
}

#else

test_suite_t test_suite_define( void )
{
	return test_socket_suite;
}

#endif

