/* tracked memory
 *
 * 2/11/99 JC
 *	- from im_open.c and callback.c
 *	- malloc tracking stuff added
 * 11/3/01 JC
 * 	- im_strncpy() added
 * 20/4/01 JC
 * 	- im_(v)snprintf() added
 * 6/7/05
 *	- more tracking for DEBUGM
 * 20/10/06
 * 	- return NULL for size <= 0
 * 11/5/06
 * 	- abort() on malloc() failure with DEBUG
 * 20/10/09
 * 	- gtkdoc comment
 * 6/11/09
 *	- im_malloc()/im_free() now call g_try_malloc()/g_free() ... removes 
 *	  confusion over whether to use im_free() or g_free() for things like 
 *	  im_header_string()
 * 21/9/11
 * 	- rename as vips_tracked_malloc() to emphasise difference from
 * 	  g_malloc()/g_free()
 */

/*

    This file is part of VIPS.
    
    VIPS is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

 */

/*

    These files are distributed with VIPS - http://www.vips.ecs.soton.ac.uk

 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif /*HAVE_CONFIG_H*/
#include <vips/intl.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include <vips/vips.h>
#include <vips/thread.h>

#ifdef WITH_DMALLOC
#include <dmalloc.h>
#endif /*WITH_DMALLOC*/

/**
 * SECTION: memory
 * @short_description: memory utilities
 * @stability: Stable
 * @include: vips/vips.h
 *
 * These functions cover two main areas.
 *
 * First, some simple utility functions over the underlying
 * g_malloc()/g_free() functions. Memory allocated and freeded using these
 * functions is interchangeable with any other glib library.
 *
 * Second, a pair of functions, vips_tracked_malloc() and vips_tracked_free()
 * which are NOT compatible. If you g_free() memory that has been allocated
 * with vips_tracked_malloc() you will see crashes. The tracked functions are
 * only suitable for large allocations internal to the library, for example
 * pixel buffers. libvips tracks the total amount of live tracked memory and
 * uses this information to decide when to trim caches.
 */

/* g_assert( 0 ) on memory errors.
#define DEBUG
 */

#ifdef DEBUG
#  warning DEBUG on in libsrc/iofuncs/memory.c
#endif /*DEBUG*/

static int vips_tracked_allocs = 0;
static size_t vips_tracked_mem = 0;
static size_t vips_tracked_mem_highwater = 0;
static GMutex *vips_tracked_mutex = NULL;

/**
 * VIPS_NEW:
 * @OBJ: allocate memory local to @OBJ, or %NULL for no auto-free
 * @T: type of thing to allocate
 *
 * Returns: A pointer of type @T *, or %NULL on error.
 */

/**
 * VIPS_ARRAY:
 * @OBJ: allocate memory local to @OBJ, or %NULL for no auto-free
 * @N: number of @T 's to allocate
 * @T: type of thing to allocate
 *
 * Returns: A pointer of type @T *, or %NULL on error.
 */

static void
vips_malloc_cb( VipsObject *object, char *buf )
{
	g_free( buf );
}

/**
 * vips_malloc:
 * @object: allocate memory local to this #VipsObject, or %NULL
 * @size: number of bytes to allocate
 *
 * g_malloc() local to @object, that is, the memory will be automatically 
 * freed for you when the object is closed. If @object is %NULL, you need to 
 * free the memory explicitly with g_free().
 *
 * This function cannot fail. See vips_tracked_malloc() if you are 
 * allocating large amounts of memory.
 *
 * See also: vips_tracked_malloc().
 *
 * Returns: a pointer to the allocated memory
 */
void *
vips_malloc( VipsObject *object, size_t size )
{
	void *buf;

	buf = g_malloc( size );

        if( object )
		g_signal_connect( object, "postclose", 
			G_CALLBACK( vips_malloc_cb ), buf );

	return( buf );
}

/**
 * vips_strdup:
 * @object: allocate memory local to this #VipsObject, or %NULL
 * @str: string to copy
 *
 * g_strdup() a string. When @object is freed, the string will be freed for
 * you.  If @object is %NULL, you need to 
 * free the memory explicitly with g_free().
 *
 * This function cannot fail. 
 *
 * See also: vips_malloc().
 *
 * Returns: a pointer to the allocated memory
 */
char *
vips_strdup( VipsObject *object, char *str )
{
	char *str_dup;

	str_dup = g_strdup( str );

        if( object )
		g_signal_connect( object, "postclose", 
			G_CALLBACK( vips_malloc_cb ), str_dup );

	return( str_dup );
}

/**
 * vips_free:
 * @buf: memory to free
 *
 * Frees memory with g_free() and returns 0. Handy for callbacks.
 *
 * See also: vips_malloc().
 *
 * Returns: 0
 */
int
vips_free( void *buf )
{
	g_free( buf );

	return( 0 );
}

/**
 * vips_tracked_free:
 * @s: memory to free
 *
 * Only use it to free
 * memory that was previously allocated with vips_tracked_malloc() with a 
 * %NULL first argument.
 *
 * See also: vips_tracked_malloc().
 */
void
vips_tracked_free( void *s )
{
	size_t size;

	/* Keep the size of the alloc in the previous 16 bytes. Ensures
	 * alignment rules are kept.
	 */
	s = (void *) ((char*)s - 16);
	size = *((size_t*)s);

	g_mutex_lock( vips_tracked_mutex );

	if( vips_tracked_allocs <= 0 ) 
		vips_warn( "vips_tracked", 
			"%s", _( "vips_free: too many frees" ) );
	vips_tracked_mem -= size;
	if( vips_tracked_mem < 0 ) 
		vips_warn( "vips_tracked", 
			"%s", _( "vips_free: too much free" ) );
	vips_tracked_allocs -= 1;

	g_mutex_unlock( vips_tracked_mutex );

	g_free( s );
}

/* g_mutex_new() is a macro.
 */
static void *
vips_tracked_mutex_new( void *data )
{
	return( g_mutex_new() );
}

static void
vips_tracked_cb( VipsObject *object, char *buf )
{
	vips_tracked_free( buf );
}

/**
 * vips_tracked_malloc:
 * @object: allocate memory local to this #VipsObject, or %NULL
 * @size: number of bytes to allocate
 *
 * Malloc local to @object, that is, the memory will be automatically 
 * freed for you when the object is closed. If @object is %NULL, you need to 
 * free the memory explicitly with vips_tracked_free().
 *
 * If allocation fails, vips_malloc() returns %NULL and 
 * sets an error message.
 *
 * You must only free the memory returned with vips_tracked_free().
 *
 * See also: vips_tracked_free(), vips_malloc().
 *
 * Returns: a pointer to the allocated memory, or %NULL on error.
 */
void *
vips_tracked_malloc( VipsObject *object, size_t size )
{
	static GOnce vips_tracked_once = G_ONCE_INIT;

        void *buf;

	vips_tracked_mutex = g_once( &vips_tracked_once, 
		vips_tracked_mutex_new, NULL );

	/* Need an extra sizeof(size_t) bytes to track 
	 * size of this block. Ask for an extra 16 to make sure we don't break
	 * alignment rules.
	 */
	size += 16;

        if( !(buf = g_try_malloc( size )) ) {
#ifdef DEBUG
		g_assert( 0 );
#endif /*DEBUG*/

		vips_error( "vips_tracked", 
			_( "out of memory --- size == %dMB" ), 
			(int) (size / (1024.0*1024.0))  );
		vips_warn( "vips_tracked", 
			_( "out of memory --- size == %dMB" ), 
			(int) (size / (1024.0*1024.0))  );

                return( NULL );
	}

	g_mutex_lock( vips_tracked_mutex );

	*((size_t *)buf) = size;
	buf = (void *) ((char *)buf + 16);

	vips_tracked_mem += size;
	if( vips_tracked_mem > vips_tracked_mem_highwater ) 
		vips_tracked_mem_highwater = vips_tracked_mem;
	vips_tracked_allocs += 1;

	g_mutex_unlock( vips_tracked_mutex );

        if( object )
		g_signal_connect( object, "postclose", 
			G_CALLBACK( vips_tracked_cb ), buf );

        return( buf );
}

/**
 * vips_alloc_get_mem:
 *
 * Returns the number of bytes currently allocated via vips_malloc() and
 * friends. vips uses this figure to decide when to start dropping cache, see
 * #VipsOperation.
 *
 * Returns: the number of currently allocated bytes
 */
size_t
vips_tracked_get_mem( void )
{
	return( vips_tracked_mem );
}

/**
 * vips_tracked_get_mem_highwater:
 *
 * Returns the largest number of bytes simultaneously allocated via 
 * vips_malloc() and friends. 
 *
 * Returns: the largest number of currently allocated bytes
 */
size_t
vips_tracked_get_mem_highwater( void )
{
	return( vips_tracked_mem_highwater );
}

/**
 * vips_tracked_get_allocs:
 *
 * Returns the number active allocations. 
 *
 * Returns: the number active allocations
 */
int
vips_tracked_get_allocs( void )
{
	return( vips_tracked_allocs );
}

