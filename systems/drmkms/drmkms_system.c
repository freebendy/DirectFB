/*
   (c) Copyright 2001-2010  The world wide DirectFB Open Source Community (directfb.org)
   (c) Copyright 2000-2004  Convergence (integrated media) GmbH

   All rights reserved.

   Written by Denis Oliver Kropp <dok@directfb.org>,
              Andreas Hundt <andi@fischlustig.de>,
              Sven Neumann <neo@directfb.org>,
              Ville Syrjälä <syrjala@sci.fi> and
              Claudio Ciccani <klan@users.sf.net>.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, write to the
   Free Software Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.
*/


//#define DIRECT_ENABLE_DEBUG

#include <config.h>

#include <fcntl.h>
#include <sys/mman.h>

#include <directfb.h>

#include <direct/mem.h>

#include <fusion/shmalloc.h>

#include <core/core.h>
#include <core/surface_pool.h>

#include <misc/conf.h>

#include "drmkms_system.h"
#include "vt.h"

#include <core/core_system.h>

D_DEBUG_DOMAIN( DRMKMS_Mode,  "DRMKMS/Mode",  "DRMKMS Mode" );
D_DEBUG_DOMAIN( DRMKMS_Layer, "DRMKMS/Layer", "DRM/KMS Layer" );

DFB_CORE_SYSTEM( drmkms )

static const char device_name[] = "/dev/dri/card0";

/**********************************************************************************************************************/

DRMKMSData *m_data;    /* FIXME: Fix Core System API to pass data in all functions. */

/**********************************************************************************************************************/

void
drmkms_page_flip_handler(int fd, unsigned int frame,
                         unsigned int sec, unsigned int usec, void *driver_data);

void
drmkms_page_flip_handler(int fd, unsigned int frame,
                         unsigned int sec, unsigned int usec, void *driver_data)
{
     DRMKMSData         *drmkms = driver_data;
     CoreSurfaceBuffer **buffer = drmkms->buffer;

     D_DEBUG_AT( DRMKMS_Layer, "%s()\n", __FUNCTION__ );

     for (int i=0; i<16;i++) {
          if (drmkms->flip_pending & (1 << i)) {
               dfb_surface_notify_display( buffer[i]->surface, buffer[i] );
               dfb_surface_buffer_unref( buffer[i] );
          }
     }

     drmkms->flip_pending = 0;
}

static void *
DRMKMS_BufferThread_Main( DirectThread *thread, void *arg )
{
     DRMKMSData *data = arg;

     D_DEBUG_AT( DRMKMS_Layer, "%s()\n", __FUNCTION__ );

     while (true) {
          direct_mutex_lock( &data->lock );

          while (!data->flip_pending) {
               D_DEBUG_AT( DRMKMS_Layer, "  -> waiting for flip to be issued\n" );

               direct_waitqueue_wait( &data->wq_flip, &data->lock );
          }

          direct_mutex_unlock( &data->lock );


          D_DEBUG_AT( DRMKMS_Layer, "  -> waiting for flip to be done\n" );

          drmHandleEvent( data->fd, &data->drmeventcontext );


          direct_mutex_lock( &data->lock );

          data->flip_pending = false;

          direct_waitqueue_broadcast( &data->wq_event );

          direct_mutex_unlock( &data->lock );
     }

     return NULL;
}


static DFBResult
InitLocal( DRMKMSData *drmkms )
{
     DFBResult   ret;
     int         i;

     drmkms->fd = open( device_name, O_RDWR );
     if (drmkms->fd < 0) {
          ret = errno2result( errno );
          D_PERROR( "DirectFB/DRMKMS: Failed to open '%s'!\n", device_name );
          return ret;
     }

#ifdef USE_GBM
     drmkms->gbm = gbm_create_device( drmkms->fd );
#else
     kms_create( drmkms->fd, &drmkms->kms);
#endif

     drmkms->resources = drmModeGetResources( drmkms->fd );
     if (!drmkms->resources) {
          D_ERROR( "DirectFB/DRMKMS: drmModeGetResources() failed!\n" );
          return DFB_INIT;
     }

     drmkms->plane_resources = drmModeGetPlaneResources( drmkms->fd );

     drmkms->screen = dfb_screens_register( NULL, drmkms, drmkmsScreenFuncs );
     drmkms->layer  = dfb_layers_register( drmkms->screen, drmkms, drmkmsLayerFuncs );


     if (drmkms->plane_resources) {
          for (i = 0; i < drmkms->plane_resources->count_planes; i++) {

               dfb_layers_register( drmkms->screen, drmkms, drmkmsPlaneLayerFuncs );
          }
     }

     return DFB_OK;
}

static DFBResult
DeinitLocal( DRMKMSData *drmkms )
{

     if (drmkms->plane_resources)
          drmModeFreePlaneResources(drmkms->plane_resources);

     if (drmkms->resources)
          drmModeFreeResources( drmkms->resources );

     if (drmkms->kms)
          kms_destroy (&drmkms->kms);

     if (drmkms->fd)
          close (drmkms->fd);

     return DFB_OK;
}

/**********************************************************************************************************************/

static void
system_get_info( CoreSystemInfo *info )
{
     info->type = CORE_DRMKMS;
     info->caps = CSCAPS_ACCELERATION;

     snprintf( info->name, DFB_CORE_SYSTEM_INFO_NAME_LENGTH, "DRM/KMS" );
}

static DFBResult
system_initialize( CoreDFB *core, void **ret_data )
{
     DFBResult              ret;
     DRMKMSData            *drmkms;
     DRMKMSDataShared      *shared;
     FusionSHMPoolShared   *pool;
     int                    ret_num;
     char                  *optionbuffer = alloca( 255 );

     D_ASSERT( m_data == NULL );

     drmkms = D_CALLOC( 1, sizeof(DRMKMSData) );
     if (!drmkms)
          return D_OOM();

     drmkms->core = core;

     pool = dfb_core_shmpool( core );

     shared = SHCALLOC( pool, 1, sizeof(DRMKMSDataShared) );
     if (!shared) {
          D_FREE( drmkms );
          return D_OOSHM();
     }

     shared->shmpool = pool;

     drmkms->shared = shared;

     m_data = drmkms;

     if (dfb_config->vt) {
          ret = dfb_vt_initialize();
          if (ret)
               return DFB_INIT;
     }

     ret = InitLocal( drmkms );
     if (ret) {
          if (dfb_config->vt)
               dfb_vt_shutdown( false );

          return ret;
     }
     *ret_data = m_data;

     dfb_surface_pool_initialize( core, &drmkmsSurfacePoolFuncs, &shared->pool );

     core_arena_add_shared_field( core, "drmkms", shared );

     if (direct_config_get("drmkms-use-prime-fd", &optionbuffer, 1, &ret_num) == DR_OK) {
          drmkms->shared->use_prime_fd = 1;
          D_INFO("DRMKMS/Init: using prime fd\n");
     }

     if (direct_config_get("drmkms-reinit-planes", &optionbuffer, 1, &ret_num) == DR_OK) {
          drmkms->shared->reinit_planes = 1;
          D_INFO("DRMKMS/Init: reinit planes workaround enabbled\n");
     }

     drmkms->drmeventcontext.version = DRM_EVENT_CONTEXT_VERSION;
     drmkms->drmeventcontext.vblank_handler = NULL;
     drmkms->drmeventcontext.page_flip_handler = drmkms_page_flip_handler;

     direct_mutex_init( &drmkms->lock );
     direct_waitqueue_init( &drmkms->wq_event );
     direct_waitqueue_init( &drmkms->wq_flip );

     drmkms->thread = direct_thread_create( DTT_CRITICAL, DRMKMS_BufferThread_Main, drmkms, "DRMKMS/Buffer" );

     return DFB_OK;
}

static DFBResult
system_join( CoreDFB *core, void **ret_data )
{
     DFBResult       ret;
     void           *tmp;
     DRMKMSData       *drmkms;
     DRMKMSDataShared *shared;

     D_ASSERT( m_data == NULL );

     if (dfb_config->vt) {
          ret = dfb_vt_join();
          if (ret)
               return DFB_INIT;
     }

     drmkms = D_CALLOC( 1, sizeof(DRMKMSData) );
     if (!drmkms)
          return D_OOM();

     drmkms->core = core;

     ret = core_arena_get_shared_field( core, "drmkms", &tmp );
     if (ret) {
          D_FREE( drmkms );
          return ret;
     }

     drmkms->shared = shared = tmp;

     ret = InitLocal( drmkms );
     if (ret)
          return ret;

     *ret_data = m_data = drmkms;

     dfb_surface_pool_join( core, shared->pool, &drmkmsSurfacePoolFuncs );

     drmkms->screen = dfb_screens_register( NULL, drmkms, drmkmsScreenFuncs );
     drmkms->layer  = dfb_layers_register( drmkms->screen, drmkms, drmkmsLayerFuncs );

     return DFB_OK;
}

static DFBResult
system_shutdown( bool emergency )
{
     DRMKMSDataShared *shared;

     D_ASSERT( m_data != NULL );

     shared = m_data->shared;
     D_ASSERT( shared != NULL );

     dfb_surface_pool_destroy( shared->pool );

     if (m_data->saved_crtc) {
          drmModeSetCrtc( m_data->fd, m_data->saved_crtc->crtc_id, m_data->saved_crtc->buffer_id, m_data->saved_crtc->x,
                          m_data->saved_crtc->y, &m_data->connector->connector_id, 1, &m_data->saved_crtc->mode );

          drmModeFreeCrtc( m_data->saved_crtc );
     }


     DeinitLocal( m_data );

     if (dfb_config->vt)
          dfb_vt_shutdown( emergency );

     SHFREE( shared->shmpool, shared );

     D_FREE( m_data );
     m_data = NULL;

     return DFB_OK;
}

static DFBResult
system_leave( bool emergency )
{
     DFBResult   ret;

     DRMKMSDataShared *shared;

     D_ASSERT( m_data != NULL );

     shared = m_data->shared;
     D_ASSERT( shared != NULL );

     dfb_surface_pool_leave( shared->pool );

     DeinitLocal( m_data );

     if (dfb_config->vt) {
          ret = dfb_vt_leave( emergency );
          if (ret)
               return ret;
     }

     D_FREE( m_data );
     m_data = NULL;

     return DFB_OK;
}

static DFBResult
system_suspend( void )
{
     D_ASSERT( m_data != NULL );

     return DFB_OK;
}

static DFBResult
system_resume( void )
{
     D_ASSERT( m_data != NULL );

     return DFB_OK;
}

static volatile void *
system_map_mmio( unsigned int    offset,
                 int             length )
{
     D_ASSERT( m_data != NULL );

     return NULL;
}

static void
system_unmap_mmio( volatile void  *addr,
                   int             length )
{
}

static int
system_get_accelerator( void )
{
     return dfb_config->accelerator;
}

static VideoMode *
system_get_modes( void )
{
     return NULL;
}

static VideoMode *
system_get_current_mode( void )
{
     return NULL;
}

static DFBResult
system_thread_init( void )
{
     return DFB_OK;
}

static bool
system_input_filter( CoreInputDevice *device,
                     DFBInputEvent   *event )
{
     if (dfb_config->vt && dfb_config->vt_switching) {
          switch (event->type) {
               case DIET_KEYPRESS:
                    if (DFB_KEY_TYPE(event->key_symbol) == DIKT_FUNCTION &&
                        event->modifiers == (DIMM_CONTROL | DIMM_ALT))
                         return dfb_vt_switch( event->key_symbol - DIKS_F1 + 1 );

                    break;

               case DIET_KEYRELEASE:
                    if (DFB_KEY_TYPE(event->key_symbol) == DIKT_FUNCTION &&
                        event->modifiers == (DIMM_CONTROL | DIMM_ALT))
                         return true;

                    break;

               default:
                    break;
          }
     }

     return false;
}

static unsigned long
system_video_memory_physical( unsigned int offset )
{
     return dfb_config->video_phys + offset;
}

static void *
system_video_memory_virtual( unsigned int offset )
{
     D_ASSERT( m_data != NULL );

     return NULL;
}

static unsigned int
system_videoram_length( void )
{
     return dfb_config->video_length;
}

static unsigned long
system_aux_memory_physical( unsigned int offset )
{
     return 0;
}

static void *
system_aux_memory_virtual( unsigned int offset )
{
     return NULL;
}

static unsigned int
system_auxram_length( void )
{
     return 0;
}

static void
system_get_busid( int *ret_bus, int *ret_dev, int *ret_func )
{
     return;
}

static int
system_surface_data_size( void )
{
     /* Return zero because shared surface data is unneeded. */
     return 0;
}

static void
system_surface_data_init( CoreSurface *surface, void *data )
{
     /* Ignore since unneeded. */
     return;
}

static void
system_surface_data_destroy( CoreSurface *surface, void *data )
{
     /* Ignore since unneeded. */
     return;
}

static void
system_get_deviceid( unsigned int *ret_vendor_id,
                     unsigned int *ret_device_id )
{
     return;
}

const drmModeModeInfo*
drmkms_find_mode( int width, int height )
{
     const drmModeModeInfo *videomode = m_data->connector->modes;
     const drmModeModeInfo *found_mode   = NULL;

     int i;
     for (i=0;i<m_data->connector->count_modes;i++) {
          if (videomode[i].hdisplay == width && videomode[i].vdisplay == height) {
                    found_mode = &videomode[i];
                    D_DEBUG_AT( DRMKMS_Mode, "Found mode %dx%d\n", width, height );

                    break;
          }
          else
               D_DEBUG_AT( DRMKMS_Mode, "Mode %dx%d does not match requested %dx%d\n", videomode[i].hdisplay, videomode[i].vdisplay, width, height );

     }

     if (!found_mode)
          D_ONCE( "no mode found for %dx%d", width, height );

     return found_mode;
}

