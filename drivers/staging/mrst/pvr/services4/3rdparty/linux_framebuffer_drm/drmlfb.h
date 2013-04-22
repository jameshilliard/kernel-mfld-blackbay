/*************************************************************************/ /*!
@Title          MRST Linux display driver structures and prototypes
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@Description    MRST Linux display driver structures and prototypes
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

#ifndef __MRSTLFB_H__
#define __MRSTLFB_H__

#include <drm/drmP.h>
#include "psb_intel_reg.h"

//#define MRST_USING_INTERRUPTS

/*
 *VDC registers and bits
 */
#define PSB_HWSTAM                0x2098
#define PSB_INSTPM                0x20C0
#define PSB_INT_IDENTITY_R        0x20A4
#define _PSB_VSYNC_PIPEB_FLAG     (1<<5)
#define _PSB_VSYNC_PIPEA_FLAG     (1<<7)
#define _PSB_IRQ_SGX_FLAG         (1<<18)
#define _PSB_IRQ_MSVDX_FLAG       (1<<19)
#define _LNC_IRQ_TOPAZ_FLAG       (1<<20)
#define PSB_INT_MASK_R            0x20A8
#define PSB_INT_ENABLE_R          0x20A0

#define MAX_SWAPCHAINS			  10

#define PVRSRV_SWAPCHAIN_ATTACHED_PLANE_NONE (0 << 0)
#define PVRSRV_SWAPCHAIN_ATTACHED_PLANE_A    (1 << 0)
#define PVRSRV_SWAPCHAIN_ATTACHED_PLANE_B    (1 << 1)
#define PVRSRV_SWAPCHAIN_ATTACHED_PLANE_C    (1 << 2)

typedef void *   MRST_HANDLE;

typedef enum tag_mrst_bool
{
	MRST_FALSE = 0,
	MRST_TRUE  = 1,
} MRST_BOOL, *MRST_PBOOL;

typedef int(* MRSTLFB_VSYNC_ISR_PFN)(struct drm_device* psDrmDevice, int iPipe);


/* MRSTLFB buffer structure */
typedef struct MRSTLFB_BUFFER_TAG
{
	/* Size of buffer */
    IMG_UINT32		             	ui32BufferSize;
	union {
		/* Physical frames list in non-contiguous buffer */
		IMG_SYS_PHYADDR             *psNonCont;
		/* Physical start frame in contiguous buffer */
		IMG_SYS_PHYADDR				sCont;
	} uSysAddr;
	/* Buffer as seen from display device (GTT address) */
	IMG_DEV_VIRTADDR             	sDevVAddr;
	/* Buffer as seen from Kernel */
    IMG_CPU_VIRTADDR             	sCPUVAddr;    
	/* Buffer sync data */
	PVRSRV_SYNC_DATA             	*psSyncData;
	/* Buffer is contiguous? */
	MRST_BOOL					 	bIsContiguous;
	/* Buffer was allocated by MRSTLFB? */
	MRST_BOOL					 	bIsAllocated;

	IMG_UINT32						ui32OwnerTaskID;
} MRSTLFB_BUFFER;


/* Flip item structure used for queuing of flips */
typedef struct MRSTLFB_VSYNC_FLIP_ITEM_TAG
{
	/* 
		command complete cookie to be passed to services 
		command complete callback function
	*/
	MRST_HANDLE      hCmdComplete;
	/* swap interval between flips */
	unsigned long    ulSwapInterval;
	/* is this item valid? */
	MRST_BOOL        bValid;
	/* has this item been flipped? */
	MRST_BOOL        bFlipped;
	/* has the flip cmd completed? */
	MRST_BOOL        bCmdCompleted;

	/* IMG structures used, to minimise API function code */
	/* replace with own structures where necessary */

	/* device virtual address of surface to flip to	(as used by Flip routine) */
	//	IMG_DEV_VIRTADDR sDevVAddr;
	MRSTLFB_BUFFER*	psBuffer;
} MRSTLFB_VSYNC_FLIP_ITEM;

/* MRSTLFB swapchain structure */
typedef struct MRSTLFB_SWAPCHAIN_TAG
{
	/* number of swap buffers */
	unsigned long       ulBufferCount;

	IMG_UINT32			ui32SwapChainID;
	IMG_UINT32			ui32SwapChainPropertyFlag;
	unsigned long			ulSwapChainGTTOffset;

	/* array of swap buffers */
	MRSTLFB_BUFFER     **ppsBuffer;

	/* swap chain length - this is greater than ulBufferCount, to allow for the system buffer being inserted into the swap chain */
	unsigned long	    ulSwapChainLength;

	/* set of vsync flip items */
	MRSTLFB_VSYNC_FLIP_ITEM	*psVSyncFlips;

	/* insert index for the internal queue of flip items */
	unsigned long       ulInsertIndex;
	
	/* remove index for the internal queue of flip items */
	unsigned long       ulRemoveIndex;

	/* jump table into PVR services */
	PVRSRV_DC_DISP2SRV_KMJTABLE	*psPVRJTable;

	/* Access to Tungsten's DRM driver routines */
	struct drm_driver         *psDrmDriver;

	/* DRM Device associated with display controller */
	struct drm_device         *psDrmDev;

	struct MRSTLFB_SWAPCHAIN_TAG *psNext;

	struct MRSTLFB_DEVINFO_TAG *psDevInfo;

} MRSTLFB_SWAPCHAIN;

/* kernel device information structure */
typedef struct MRSTLFB_DEVINFO_TAG
{
	unsigned int           uiDeviceID;

	struct drm_device 	*psDrmDevice;

	/* system surface info */

	MRSTLFB_BUFFER          sSystemBuffer;

	/* jump table into PVR services */
	PVRSRV_DC_DISP2SRV_KMJTABLE	sPVRJTable;
	
	/* jump table into DC */
	PVRSRV_DC_SRV2DISP_KMJTABLE	sDCJTable;

	/* ref count */
	unsigned long           ulRefCount;

	MRSTLFB_SWAPCHAIN      *psCurrentSwapChain;

	MRSTLFB_SWAPCHAIN      *apsSwapChains[MAX_SWAPCHAINS];

	IMG_UINT32	   	ui32SwapChainNum;

	/* registers used to control flipping */
	void *pvRegs;

	/* Reference counter for SetDCState */
	unsigned long ulSetFlushStateRefCount;

	/* True if swap chain is being flushed */
	MRST_BOOL           bFlushCommands;

	/* True if screen is blanked */
	MRST_BOOL           bBlanked;

	/* pointer to linux frame buffer information structure */
	struct fb_info         *psLINFBInfo;

	/* Linux Framebuffer event notification block */
	struct notifier_block   sLINNotifBlock;

	/* Swap chain lock */
	spinlock_t             sSwapChainLock;

	/* IMG structures used, to minimise API function code */
	/* replace with own structures where necessary */

	/* Address of the surface being displayed */
	IMG_DEV_VIRTADDR	sDisplayDevVAddr;

	DISPLAY_INFO            sDisplayInfo;

	/* Display format */
	DISPLAY_FORMAT          sDisplayFormat;
	
	/* Display dimensions */
	DISPLAY_DIMS            sDisplayDim;

	IMG_UINT32		ui32MainPipe;

	/* Driver is suspended */
	MRST_BOOL bSuspended;

	/* The system has switched away from our VT */
	MRST_BOOL bLeaveVT;

	/* Address of last flip */
	unsigned long ulLastFlipAddr;

	/* The last flip address is valid */
	MRST_BOOL bLastFlipAddrValid;
}  MRSTLFB_DEVINFO;

/* DEBUG only printk */
#ifdef	DEBUG
#define	DEBUG_PRINTK(x) printk x
#else
#define	DEBUG_PRINTK(x)
#endif

#define DISPLAY_DEVICE_NAME "PowerVR Moorestown Linux Display Driver"
#define	DRVNAME	"mrstlfb"
#define	DEVNAME	DRVNAME
#define	DRIVER_PREFIX DRVNAME

/*!
 *****************************************************************************
 * Error values
 *****************************************************************************/
typedef enum _MRST_ERROR_
{
	MRST_OK                             =  0,
	MRST_ERROR_GENERIC                  =  1,
	MRST_ERROR_OUT_OF_MEMORY            =  2,
	MRST_ERROR_TOO_FEW_BUFFERS          =  3,
	MRST_ERROR_INVALID_PARAMS           =  4,
	MRST_ERROR_INIT_FAILURE             =  5,
	MRST_ERROR_CANT_REGISTER_CALLBACK   =  6,
	MRST_ERROR_INVALID_DEVICE           =  7,
	MRST_ERROR_DEVICE_REGISTER_FAILED   =  8
} MRST_ERROR;


#ifndef UNREFERENCED_PARAMETER
#define	UNREFERENCED_PARAMETER(param) (param) = (param)
#endif

MRST_ERROR MRSTLFBInit(struct drm_device * dev);
MRST_ERROR MRSTLFBDeinit(void);

MRST_ERROR MRSTLFBAllocBuffer(struct MRSTLFB_DEVINFO_TAG *psDevInfo, IMG_UINT32 ui32Size, MRSTLFB_BUFFER **ppBuffer);
MRST_ERROR MRSTLFBFreeBuffer(struct MRSTLFB_DEVINFO_TAG *psDevInfo, MRSTLFB_BUFFER **ppBuffer);

/* OS Specific APIs */
void *MRSTLFBAllocKernelMem(unsigned long ulSize);
void MRSTLFBFreeKernelMem(void *pvMem);
MRST_ERROR MRSTLFBGetLibFuncAddr(char *szFunctionName, PFN_DC_GET_PVRJTABLE *ppfnFuncTable);
MRST_ERROR MRSTLFBInstallVSyncISR (MRSTLFB_DEVINFO *psDevInfo, MRSTLFB_VSYNC_ISR_PFN pVsyncHandler);
MRST_ERROR MRSTLFBUninstallVSyncISR(MRSTLFB_DEVINFO *psDevInfo);

void MRSTLFBEnableVSyncInterrupt(MRSTLFB_DEVINFO *psDevInfo);
void MRSTLFBDisableVSyncInterrupt(MRSTLFB_DEVINFO *psDevInfo);

void MRSTLFBFlipToSurface(MRSTLFB_DEVINFO *psDevInfo,  unsigned long uiAddr);

void MRSTLFBSuspend(void);
void MRSTLFBResume(void);

MRST_ERROR MRSTLFBChangeSwapChainProperty(unsigned long *psSwapChainGTTOffset,
		unsigned long ulSwapChainGTTSize, IMG_INT32 i32Pipe);

#endif /* __MRSTLFB_H__ */

/******************************************************************************
 End of file (mrstlfb.h)
******************************************************************************/
