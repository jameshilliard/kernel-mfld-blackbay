/*************************************************************************/ /*!
@Title          MRST Linux display driver linux-specific functions
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@License        Dual MIT/GPLv2

The contents of this file are subject to the MIT license as set out below.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

Alternatively, the contents of this file may be used under the terms of
the GNU General Public License Version 2 ("GPL") in which case the provisions
of GPL are applicable instead of those above.

If you wish to allow use of your version of this file only under the terms of
GPL, and not to allow others to use your version of this file under the terms
of the MIT license, indicate your decision by deleting the provisions above
and replace them with the notice and other provisions required by GPL as set
out in the file called "GPL-COPYING" included in this distribution. If you do
not delete the provisions above, a recipient may use your version of this file
under the terms of either the MIT license or GPL.

This License is also included in this distribution in the file called
"MIT-COPYING".

EXCEPT AS OTHERWISE STATED IN A NEGOTIATED AGREEMENT: (A) THE SOFTWARE IS
PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
PURPOSE AND NONINFRINGEMENT; AND (B) IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/ /**************************************************************************/

/* Mainly adapted from dc_omaplfb_linux */

/**************************************************************************
 The 3rd party driver is a specification of an API to integrate the IMG POWERVR
 Services driver with 3rd Party display hardware.  It is NOT a specification for
 a display controller driver, rather a specification to extend the API for a
 pre-existing driver for the display hardware.

 The 3rd party driver interface provides IMG POWERVR client drivers (e.g. PVR2D)
 with an API abstraction of the system's underlying display hardware, allowing
 the client drivers to indirectly control the display hardware and access its
 associated memory.
 
 Functions of the API include
 - query primary surface attributes (width, height, stride, pixel format, CPU
     physical and virtual address)
 - swap/flip chain creation and subsequent query of surface attributes
 - asynchronous display surface flipping, taking account of asynchronous read
 (flip) and write (render) operations to the display surface

 Note: having queried surface attributes the client drivers are able to map the
 display memory to any IMG POWERVR Services device by calling
 PVRSRVMapDeviceClassMemory with the display surface handle.

 This code is intended to be an example of how a pre-existing display driver may
 be extended to support the 3rd Party Display interface to POWERVR Services
 - IMG is not providing a display driver implementation.
 **************************************************************************/

#include <linux/version.h>

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,38))
#ifndef AUTOCONF_INCLUDED
#include <linux/config.h>
#endif
#endif

#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/interrupt.h>

#include <drm/drmP.h>

#include <asm/io.h>

#include "img_defs.h"
#include "servicesext.h"
#include "kerneldisplay.h"
#include "pvrmodule.h"
#include "pvr_drm.h"
#include "drmlfb.h"


#include "psb_drv.h"

#if !defined(SUPPORT_DRI_DRM)
#error "SUPPORT_DRI_DRM must be set"
#endif

#define	MAKESTRING(x) # x

#if !defined(DISPLAY_CONTROLLER)
#define DISPLAY_CONTROLLER pvrlfb
#endif


#define unref__ __attribute__ ((unused))


extern int fb_idx;

void *MRSTLFBAllocKernelMem(unsigned long ulSize)
{
	return kmalloc(ulSize, GFP_KERNEL);
}

void MRSTLFBFreeKernelMem(void *pvMem)
{
	kfree(pvMem);
}


MRST_ERROR MRSTLFBGetLibFuncAddr (char *szFunctionName, PFN_DC_GET_PVRJTABLE *ppfnFuncTable)
{
	if(strcmp("PVRGetDisplayClassJTable", szFunctionName) != 0)
	{
		return (MRST_ERROR_INVALID_PARAMS);
	}

	/* Nothing to do - should be exported from pvrsrv.ko */
	*ppfnFuncTable = PVRGetDisplayClassJTable;

	return (MRST_OK);
}

static void MRSTLFBVSyncWriteReg(MRSTLFB_DEVINFO *psDevInfo, unsigned long ulOffset, unsigned long ulValue)
{

	void *pvRegAddr = (void *)(psDevInfo->pvRegs + ulOffset);
	mb();
	iowrite32(ulValue, pvRegAddr);
}

/* function to enable VSync interrupts */
void MRSTLFBEnableVSyncInterrupt(MRSTLFB_DEVINFO * psDevinfo)
{
#if defined(MRST_USING_INTERRUPTS)	
	if( drm_vblank_get( psDevinfo->psDrmDevice , psDevinfo->ui32MainPipe ) )
	{
		DEBUG_PRINTK((KERN_INFO DRIVER_PREFIX	"Couldn't get vsync enabled"));
	}
#endif
}

/* function to disable VSync interrupts */
void MRSTLFBDisableVSyncInterrupt(MRSTLFB_DEVINFO * psDevinfo)
{
#if defined(MRST_USING_INTERRUPTS)	
	drm_vblank_put( psDevinfo->psDrmDevice,  psDevinfo->ui32MainPipe );    
#endif
}

#if defined(MRST_USING_INTERRUPTS)
MRST_ERROR MRSTLFBInstallVSyncISR(MRSTLFB_DEVINFO *psDevInfo, MRSTLFB_VSYNC_ISR_PFN pVsyncHandler)
{
	struct drm_psb_private *dev_priv =
	    (struct drm_psb_private *) psDevInfo->psDrmDevice->dev_private;
	dev_priv->psb_vsync_handler = pVsyncHandler;
	return (MRST_OK);
}


MRST_ERROR MRSTLFBUninstallVSyncISR(MRSTLFB_DEVINFO	*psDevInfo)
{
	struct drm_psb_private *dev_priv =
	    (struct drm_psb_private *) psDevInfo->psDrmDevice->dev_private;
	dev_priv->psb_vsync_handler = NULL;
	return (MRST_OK);
}
#endif //(MRST_USING_INTERRUPTS)


/* Flip to buffer function : 
 * note - according to the Moorestown manuals the value in DSPASURF is only updated after the next VSync ('double buffering') 
 *      - We are passed a 'device virtual address' ie GTT address. 0 points to the start of the stolen area
 */
void MRSTLFBFlipToSurface(MRSTLFB_DEVINFO *psDevInfo,  unsigned long uiAddr)
{
	int dspbase = (psDevInfo->ui32MainPipe == 0 ? MRST_DSPABASE : MRST_DSPBBASE);
	int dspsurf = (psDevInfo->ui32MainPipe == 0 ? DSPASURF : DSPBSURF);

	if (ospm_power_using_hw_begin(OSPM_DISPLAY_ISLAND, true))
	{
		if (IS_MRST(psDevInfo->psDrmDevice)) {
			MRSTLFBVSyncWriteReg(psDevInfo, dspsurf, uiAddr);
		} else {
			MRSTLFBVSyncWriteReg(psDevInfo, dspbase, uiAddr);
		}
		ospm_power_using_hw_end(OSPM_DISPLAY_ISLAND);
	}
}


/*****************************************************************************
 Function Name:	MRSTLFB_Init
 Description  :	Insert the driver into the kernel.

*****************************************************************************/
int PVR_DRM_MAKENAME(DISPLAY_CONTROLLER, _Init)(struct drm_device unref__ *dev)
{
	if(MRSTLFBInit(dev) != MRST_OK)
	{
		printk(KERN_WARNING DRIVER_PREFIX ": %s: MRSTLFBInit failed\n", __FUNCTION__);
		return -ENODEV;
	}

	return 0;
}

/*****************************************************************************
 Function Name:	MRSTLFB_Cleanup
 Description  :	Remove the driver from the kernel.

*****************************************************************************/
void PVR_DRM_MAKENAME(DISPLAY_CONTROLLER, _Cleanup)(struct drm_device unref__ *dev)
{    
	if(MRSTLFBDeinit() != MRST_OK)
	{
		printk(KERN_WARNING DRIVER_PREFIX ": %s: can't deinit device\n", __FUNCTION__);
	}
}

/*****************************************************************************
 Function Name: MRSTLFB_Suspend
 Description : Suspend the driver.
*****************************************************************************/
int PVR_DRM_MAKENAME(DISPLAY_CONTROLLER, _Suspend)(struct drm_device unref__ *dev)
{
	MRSTLFBSuspend();

	return 0;
}

/*****************************************************************************
 Function Name: MRSTLFB_Resume
 Description : Resume the driver.
*****************************************************************************/
int PVR_DRM_MAKENAME(DISPLAY_CONTROLLER, _Resume)(struct drm_device unref__ *dev)
{
	MRSTLFBResume();

	return 0;
}

