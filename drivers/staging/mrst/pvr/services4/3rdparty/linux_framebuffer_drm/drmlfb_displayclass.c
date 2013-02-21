/*************************************************************************/ /*!
@Title          MRST Linux display driver display-specific functions
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


/*
 * Moorestown Linux 3rd party display driver.
 * This is based on the OMAP 3rd party display driver including extensions to support flipping.
 * This OMAP 3rd party driver is in turn based on the Generic PVR Linux Framebuffer 3rd party display

 */

#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/console.h>
#include <linux/fb.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/notifier.h>
#include <linux/spinlock.h>

/* IMG services headers */
#include "img_defs.h"
#include "servicesext.h"
#include "kerneldisplay.h"
#include "drmlfb.h"

#include "psb_drv.h"
#include "psb_fb.h"

#if !defined(SUPPORT_DRI_DRM)
#error "SUPPORT_DRI_DRM must be set"
#endif

static void *gpvAnchor;


#define MRSTLFB_COMMAND_COUNT		1

/* top level 'hook ptr' */
static PFN_DC_GET_PVRJTABLE pfnGetPVRJTable = 0;

/* returns anchor pointer */
static MRSTLFB_DEVINFO * GetAnchorPtr(void)
{
	return (MRSTLFB_DEVINFO *)gpvAnchor;
}

/* sets anchor pointer */
static void SetAnchorPtr(MRSTLFB_DEVINFO *psDevInfo)
{
	gpvAnchor = (void*)psDevInfo;
}

/* Flip to buffer, saving buffer address for a possible restore */
static void MRSTLFBFlip(MRSTLFB_DEVINFO *psDevInfo, MRSTLFB_BUFFER *psBuffer)
{
	unsigned long ulAddr = (unsigned long)psBuffer->sDevVAddr.uiAddr;

	if (!psDevInfo->bSuspended && !psDevInfo->bLeaveVT)
	{
		MRSTLFBFlipToSurface(psDevInfo, ulAddr);
	}

	psDevInfo->ulLastFlipAddr = ulAddr;
	psDevInfo->bLastFlipAddrValid = MRST_TRUE;
}

/*
 * Flip to the last buffer flipped.  Intended for restoring the last buffer
 * flipped to after the device resumes, or on re-entering our VT.
 */
static void MRSTLFBRestoreLastFlip(MRSTLFB_DEVINFO *psDevInfo)
{
	if (!psDevInfo->bSuspended && !psDevInfo->bLeaveVT)
	{
		if (psDevInfo->bLastFlipAddrValid)
		{
			MRSTLFBFlipToSurface(psDevInfo, psDevInfo->ulLastFlipAddr);
		}
	}
}

/* Prevent a restore of the last buffer flipped */
static void MRSTLFBClearSavedFlip(MRSTLFB_DEVINFO *psDevInfo)
{
	psDevInfo->bLastFlipAddrValid = MRST_FALSE;
}

/* 
	Function to flush all items out of the VSYNC queue.
	If bFlip is false, we don't want the items to be displayed.
*/	
static void FlushInternalVSyncQueue(MRSTLFB_SWAPCHAIN *psSwapChain, MRST_BOOL bFlip)
{
	MRSTLFB_VSYNC_FLIP_ITEM *psFlipItem;
	unsigned long            ulMaxIndex;
	unsigned long            i;

	/* Need to flush any flips now pending in internal queue */
	psFlipItem = &psSwapChain->psVSyncFlips[psSwapChain->ulRemoveIndex];
	ulMaxIndex = psSwapChain->ulSwapChainLength - 1;

	for(i = 0; i < psSwapChain->ulSwapChainLength; i++)
	{
		if (psFlipItem->bValid == MRST_FALSE)
		{
			continue;
		}

		DEBUG_PRINTK((KERN_INFO DRIVER_PREFIX ": FlushInternalVSyncQueue: Flushing swap buffer (index %lu)\n", psSwapChain->ulRemoveIndex));

		if(psFlipItem->bFlipped == MRST_FALSE)
		{
			if (bFlip)
			{
				/*
				 * Flip to new surface, flip latches on next
				 * interrupt.
				 */
				MRSTLFBFlip(psSwapChain->psDevInfo, psFlipItem->psBuffer);
			}
		}
		/* Command complete handler.
		 * Allows dependencies for outstanding flips to be updated.
		 * Doesn't matter that vsync interrupts have been disabled. 
		 */
		if(psFlipItem->bCmdCompleted == MRST_FALSE)
		{
			DEBUG_PRINTK((KERN_INFO DRIVER_PREFIX ": FlushInternalVSyncQueue: Calling command complete for swap buffer (index %lu)\n", psSwapChain->ulRemoveIndex));

			psSwapChain->psPVRJTable->pfnPVRSRVCmdComplete((IMG_HANDLE)psFlipItem->hCmdComplete, MRST_TRUE);
		}

		/* advance remove index */
		psSwapChain->ulRemoveIndex++;
		
		if(psSwapChain->ulRemoveIndex > ulMaxIndex)
		{
			psSwapChain->ulRemoveIndex = 0;
		}

		/* clear item state */
		psFlipItem->bFlipped = MRST_FALSE;
		psFlipItem->bCmdCompleted = MRST_FALSE;
		psFlipItem->bValid = MRST_FALSE;
		
		/* update to next flip item */
		psFlipItem = &psSwapChain->psVSyncFlips[psSwapChain->ulRemoveIndex];
	}

	psSwapChain->ulInsertIndex = 0;
	psSwapChain->ulRemoveIndex = 0;
}

static void DRMLFBFlipBuffer(MRSTLFB_DEVINFO *psDevInfo, MRSTLFB_SWAPCHAIN *psSwapChain, MRSTLFB_BUFFER *psBuffer) 
{
	if(psSwapChain != NULL) 
	{
		if(psDevInfo->psCurrentSwapChain != NULL)
		{
			/* Flush old swapchain */
			if(psDevInfo->psCurrentSwapChain != psSwapChain) 
				FlushInternalVSyncQueue(psDevInfo->psCurrentSwapChain, MRST_FALSE);
		}
		psDevInfo->psCurrentSwapChain = psSwapChain;
	}

	/* Flip */
	MRSTLFBFlip(psDevInfo, psBuffer);
}

/*
 * Set the swap chain flush state.  If flushing is enabled, flips are
 * executed straight away, and are never locked to the VSync interrupt.
 * This mode can be enabled by Services, via the SetDCState function,
 * or internally, if the screen goes blank.  In the latter case, there
 * is no longer any VSync interrupt to synchronise flips to, hence
 * flush mode is enabled to stop applications from hanging, waiting
 * for flips to take place.
 */
static void SetFlushStateNoLock(MRSTLFB_DEVINFO* psDevInfo,
                                        MRST_BOOL bFlushState)
{
	if (bFlushState)
	{
		if (psDevInfo->ulSetFlushStateRefCount == 0)
		{
			psDevInfo->bFlushCommands = MRST_TRUE;
			if (psDevInfo->psCurrentSwapChain != NULL)
			{
				FlushInternalVSyncQueue(psDevInfo->psCurrentSwapChain, MRST_TRUE);
			}
		}
		psDevInfo->ulSetFlushStateRefCount++;
	}
	else
	{
		if (psDevInfo->ulSetFlushStateRefCount != 0)
		{
			psDevInfo->ulSetFlushStateRefCount--;
			if (psDevInfo->ulSetFlushStateRefCount == 0)
			{
				psDevInfo->bFlushCommands = MRST_FALSE;
			}
		}
	}
}

static IMG_VOID SetFlushState(MRSTLFB_DEVINFO* psDevInfo,
                                      MRST_BOOL bFlushState)
{
	unsigned long ulLockFlags;

	spin_lock_irqsave(&psDevInfo->sSwapChainLock, ulLockFlags);

	SetFlushStateNoLock(psDevInfo, bFlushState);

	spin_unlock_irqrestore(&psDevInfo->sSwapChainLock, ulLockFlags);
}

/*
 * SetDCState
 * Called from services.
 */
static IMG_VOID SetDCState(IMG_HANDLE hDevice, IMG_UINT32 ui32State)
{
	MRSTLFB_DEVINFO *psDevInfo = (MRSTLFB_DEVINFO *)hDevice;

	switch (ui32State)
	{
		case DC_STATE_FLUSH_COMMANDS:
			SetFlushState(psDevInfo, MRST_TRUE);
			break;
		case DC_STATE_NO_FLUSH_COMMANDS:
			SetFlushState(psDevInfo, MRST_FALSE);
			break;
		default:
			break;
	}

	return;
}

/* Framebuffer event notification handler */
static int FrameBufferEvents(struct notifier_block *psNotif,
							 unsigned long event, void *data)
{
#if defined(MRST_USE_PRIVATE_FB_NOTIFIER)
	if (PSB_FB_EVENT_BLANK == event)
#else
	if (FB_EVENT_BLANK == event)
#endif
	{
		MRSTLFB_DEVINFO *psDevInfo = GetAnchorPtr();
		MRST_BOOL bBlanked;
#if defined(MRST_USE_PRIVATE_FB_NOTIFIER)
		struct psb_fb_event *psFBEvent = (struct psb_fb_event *)data;

		bBlanked = (*(IMG_INT *)psFBEvent->pvData != 0) ? MRST_TRUE: MRST_FALSE;
#else
		struct fb_event *psFBEvent = (struct fb_event *)data;

		bBlanked = (*(IMG_INT *)psFBEvent->data != 0) ? MRST_TRUE: MRST_FALSE;
#endif

		if (bBlanked != psDevInfo->bBlanked)
		{
			psDevInfo->bBlanked = bBlanked;

			SetFlushState(psDevInfo, bBlanked);
		}
	}

	return 0;
}


/* Unblank the screen */
static MRST_ERROR UnblankDisplay(MRSTLFB_DEVINFO *psDevInfo)
{
	int res;

	console_lock();
	//acquire_console_sem();
	res = fb_blank(psDevInfo->psLINFBInfo, 0);
	console_unlock();
	//release_console_sem();
	if (res != 0)
	{
		printk(KERN_WARNING DRIVER_PREFIX
			": fb_blank failed (%d)", res);
		return (MRST_ERROR_GENERIC);
	}

	return (MRST_OK);
}

/*
 * EnableLFBEventNotification
 * Set up Framebuffer event notification.
 */
static MRST_ERROR EnableLFBEventNotification(MRSTLFB_DEVINFO *psDevInfo)
{
	int			res;
	MRST_ERROR	eError;

	/* Set up Framebuffer event notification */
	memset(&psDevInfo->sLINNotifBlock, 0, sizeof(psDevInfo->sLINNotifBlock));

	psDevInfo->sLINNotifBlock.notifier_call = FrameBufferEvents;
	psDevInfo->bBlanked = MRST_FALSE;

#if defined(MRST_USE_PRIVATE_FB_NOTIFIER)
	/* X Server power management events do not blank/unblank the screen using fb_blank, so
	 * the Linux Framebuffer event notification will not notify clients of such events. A
	 * private event notification mechanism is used instead.*/
	res = psb_fb_register_client(psDevInfo->psDrmDevice, &psDevInfo->sLINNotifBlock);
#else
	res = fb_register_client(&psDevInfo->sLINNotifBlock);
#endif
	if (res != 0)
	{
		DEBUG_PRINTK((KERN_WARNING DRIVER_PREFIX " - Failed to register framebuffer notifier (%d).\n", res));

		return (MRST_ERROR_GENERIC);
	}

	eError = UnblankDisplay(psDevInfo);
	if (eError != MRST_OK)
	{
		DEBUG_PRINTK((KERN_WARNING DRIVER_PREFIX
			": UnblankDisplay failed (%d)", eError));
		return eError;
	}

	return (MRST_OK);
}

/*
 * DisableLFBEventNotification
 * Disable Framebuffer event notification.
 */
static MRST_ERROR DisableLFBEventNotification(MRSTLFB_DEVINFO *psDevInfo)
{
	int res;

/* Unregister for Framebuffer events */
#if defined(MRST_USE_PRIVATE_FB_NOTIFIER)
	/* X Server power management events do not blank/unblank the screen using fb_blank, so
	 * the Linux Framebuffer event notification will not notify clients of such events. A
	 * private event notification mechanism is used instead.*/
	res = psb_fb_unregister_client(psDevInfo->psDrmDevice, &psDevInfo->sLINNotifBlock);
#else
	res = fb_unregister_client(&psDevInfo->sLINNotifBlock);
#endif
	if (res != 0)
	{
		DEBUG_PRINTK((KERN_WARNING DRIVER_PREFIX " - Failed to unregister framebuffer notifier (%d).\n", res));

		return (MRST_ERROR_GENERIC);
	}

	return (MRST_OK);
}

/*
 * OpenDCDevice
 * Called from services.
 */
static PVRSRV_ERROR OpenDCDevice(IMG_UINT32 ui32DeviceID,
                                 IMG_HANDLE *phDevice,
                                 PVRSRV_SYNC_DATA* psSystemBufferSyncData)
{
	MRSTLFB_DEVINFO *psDevInfo;
	MRST_ERROR eError;

	UNREFERENCED_PARAMETER(ui32DeviceID);

	psDevInfo = GetAnchorPtr();

	/* store the system surface sync data */
	psDevInfo->sSystemBuffer.psSyncData = psSystemBufferSyncData;

	psDevInfo->ulSetFlushStateRefCount = 0;
	psDevInfo->bFlushCommands = MRST_FALSE;

	eError = EnableLFBEventNotification(psDevInfo);
	if (eError != MRST_OK)
	{
		printk(KERN_WARNING DRIVER_PREFIX ": Couldn't enable framebuffer event notification\n");
		return PVRSRV_ERROR_UNABLE_TO_OPEN_DC_DEVICE;
	}

	/* return handle to the devinfo */
	*phDevice = (IMG_HANDLE)psDevInfo;
	
	return (PVRSRV_OK);
}

/*
 * CloseDCDevice
 * Called from services.
 */
static PVRSRV_ERROR CloseDCDevice(IMG_HANDLE hDevice)
{
	MRSTLFB_DEVINFO *psDevInfo = (MRSTLFB_DEVINFO *)hDevice;
	MRST_ERROR eError;

	eError = DisableLFBEventNotification(psDevInfo);
	if (eError != MRST_OK)
	{
		printk(KERN_WARNING DRIVER_PREFIX ": Couldn't disable framebuffer event notification\n");
		return PVRSRV_ERROR_UNABLE_TO_REMOVE_DEVICE;
	}

	return (PVRSRV_OK);
}

/*
 * EnumDCFormats
 * Called from services.
 */
static PVRSRV_ERROR EnumDCFormats(IMG_HANDLE hDevice,
                                  IMG_UINT32 *pui32NumFormats,
                                  DISPLAY_FORMAT *psFormat)
{
	MRSTLFB_DEVINFO	*psDevInfo;
	
	if(!hDevice || !pui32NumFormats)
	{
		return (PVRSRV_ERROR_INVALID_PARAMS);
	}

	psDevInfo = (MRSTLFB_DEVINFO*)hDevice;
	
	*pui32NumFormats = 1;
	
	if(psFormat)
	{
		psFormat[0] = psDevInfo->sDisplayFormat;
	}

	return (PVRSRV_OK);
}

/*
 * EnumDCDims
 * Called from services.
 */
static PVRSRV_ERROR EnumDCDims(IMG_HANDLE hDevice, 
                               DISPLAY_FORMAT *psFormat,
                               IMG_UINT32 *pui32NumDims,
                               DISPLAY_DIMS *psDim)
{
	MRSTLFB_DEVINFO	*psDevInfo;

	if(!hDevice || !psFormat || !pui32NumDims)
	{
		return (PVRSRV_ERROR_INVALID_PARAMS);
	}

	psDevInfo = (MRSTLFB_DEVINFO*)hDevice;

	*pui32NumDims = 1;

	/* No need to look at psFormat; there is only one */
	if(psDim)
	{
		psDim[0] = psDevInfo->sDisplayDim;
	}
	
	return (PVRSRV_OK);
}


/*
 * GetDCSystemBuffer
 * Called from services.
 */
static PVRSRV_ERROR GetDCSystemBuffer(IMG_HANDLE hDevice, IMG_HANDLE *phBuffer)
{
	MRSTLFB_DEVINFO	*psDevInfo;
	
	if(!hDevice || !phBuffer)
	{
		return (PVRSRV_ERROR_INVALID_PARAMS);
	}

	psDevInfo = (MRSTLFB_DEVINFO*)hDevice;

	//printk(" GetDCSystemBuffer : %p\n", (void*) psDevInfo->sSystemBuffer.sSysAddr.uiAddr);
	
	*phBuffer = (IMG_HANDLE)&psDevInfo->sSystemBuffer;

	return (PVRSRV_OK);
}


/*
 * GetDCInfo
 * Called from services.
 */
static PVRSRV_ERROR GetDCInfo(IMG_HANDLE hDevice, DISPLAY_INFO *psDCInfo)
{
	MRSTLFB_DEVINFO	*psDevInfo;
	
	if(!hDevice || !psDCInfo)
	{
		return (PVRSRV_ERROR_INVALID_PARAMS);
	}

	psDevInfo = (MRSTLFB_DEVINFO*)hDevice;

	*psDCInfo = psDevInfo->sDisplayInfo;

	return (PVRSRV_OK);
}

/*
 * GetDCBufferAddr
 * Called from services.
 */
static PVRSRV_ERROR GetDCBufferAddr(IMG_HANDLE        hDevice,
                                    IMG_HANDLE        hBuffer, 
                                    IMG_SYS_PHYADDR   **ppsSysAddr,
                                    IMG_SIZE_T        *pui32ByteSize,
                                    IMG_VOID          **ppvCpuVAddr,
                                    IMG_HANDLE        *phOSMapInfo,
                                    IMG_BOOL          *pbIsContiguous,
	                            IMG_UINT32	      *pui32TilingStride)
{
	MRSTLFB_DEVINFO	*psDevInfo;
	MRSTLFB_BUFFER *psSystemBuffer;

	UNREFERENCED_PARAMETER(pui32TilingStride);

	if(!hDevice)
	{
		return (PVRSRV_ERROR_INVALID_PARAMS);
	}
	psDevInfo = (MRSTLFB_DEVINFO*)hDevice;
	
	if(!hBuffer)
	{
		return (PVRSRV_ERROR_INVALID_PARAMS);
	}
	psSystemBuffer = (MRSTLFB_BUFFER *)hBuffer;

	if (!ppsSysAddr)
	{
		return (PVRSRV_ERROR_INVALID_PARAMS);
	}

	if( psSystemBuffer->bIsContiguous ) 
		*ppsSysAddr = &psSystemBuffer->uSysAddr.sCont;
	else
		*ppsSysAddr = psSystemBuffer->uSysAddr.psNonCont;

	if (!pui32ByteSize)
	{
		return (PVRSRV_ERROR_INVALID_PARAMS);
	}
	*pui32ByteSize = psSystemBuffer->ui32BufferSize;

	if (ppvCpuVAddr)
	{
		*ppvCpuVAddr = psSystemBuffer->sCPUVAddr;
	}

	if (phOSMapInfo)
	{
		*phOSMapInfo = (IMG_HANDLE)0;
	}

	if (pbIsContiguous)
	{
		*pbIsContiguous = psSystemBuffer->bIsContiguous;
	}

	return (PVRSRV_OK);
}

/*
 * 
 * Called from services.
 */
static PVRSRV_ERROR CreateDCSwapChain(IMG_HANDLE hDevice,
                                      IMG_UINT32 ui32Flags,
                                      DISPLAY_SURF_ATTRIBUTES *psDstSurfAttrib,
                                      DISPLAY_SURF_ATTRIBUTES *psSrcSurfAttrib,
                                      IMG_UINT32 ui32BufferCount,
                                      PVRSRV_SYNC_DATA **ppsSyncData,
                                      IMG_UINT32 ui32OEMFlags,
                                      IMG_HANDLE *phSwapChain,
                                      IMG_UINT32 *pui32SwapChainID)
{
	MRSTLFB_DEVINFO	*psDevInfo;
	MRSTLFB_SWAPCHAIN *psSwapChain;
	MRSTLFB_BUFFER **ppsBuffer;
	MRSTLFB_VSYNC_FLIP_ITEM *psVSyncFlips;
	IMG_UINT32 i;
	IMG_UINT32 iSCId = MAX_SWAPCHAINS;
	PVRSRV_ERROR eError = PVRSRV_ERROR_NOT_SUPPORTED;
	unsigned long ulLockFlags;
	struct drm_device* psDrmDev;
	unsigned long ulSwapChainLength;

	UNREFERENCED_PARAMETER(ui32OEMFlags);
	
	/* Check parameters */
	if(!hDevice
	|| !psDstSurfAttrib
	|| !psSrcSurfAttrib
	|| !ppsSyncData
	|| !phSwapChain)
	{
		return (PVRSRV_ERROR_INVALID_PARAMS);
	}

	psDevInfo = (MRSTLFB_DEVINFO*)hDevice;
		
	/* Check the buffer count */
	if(ui32BufferCount > psDevInfo->sDisplayInfo.ui32MaxSwapChainBuffers)
	{
		return (PVRSRV_ERROR_TOOMANYBUFFERS);
	}
	
	/*
	 * PVR Services swaps to the system buffer by inserting it in
	 * the swap chain, so ensure the swap chain is long enough.
	 */
	ulSwapChainLength = ui32BufferCount + 1;

	/* 
	 *	Verify the DST/SRC attributes,
	 *	SRC/DST must match the current display mode config
	*/
	if(psDstSurfAttrib->pixelformat != psDevInfo->sDisplayFormat.pixelformat
	|| psDstSurfAttrib->sDims.ui32ByteStride != psDevInfo->sDisplayDim.ui32ByteStride
	|| psDstSurfAttrib->sDims.ui32Width != psDevInfo->sDisplayDim.ui32Width
	|| psDstSurfAttrib->sDims.ui32Height != psDevInfo->sDisplayDim.ui32Height)
	{
		/* DST doesn't match the current mode */
		return (PVRSRV_ERROR_INVALID_PARAMS);
	}		

	if(psDstSurfAttrib->pixelformat != psSrcSurfAttrib->pixelformat
	|| psDstSurfAttrib->sDims.ui32ByteStride != psSrcSurfAttrib->sDims.ui32ByteStride
	|| psDstSurfAttrib->sDims.ui32Width != psSrcSurfAttrib->sDims.ui32Width
	|| psDstSurfAttrib->sDims.ui32Height != psSrcSurfAttrib->sDims.ui32Height)
	{
		/* DST doesn't match the SRC */
		return (PVRSRV_ERROR_INVALID_PARAMS);
	}		

	/* check flags if implementation requires them */
	UNREFERENCED_PARAMETER(ui32Flags);
	
	/* create a swapchain structure */
	psSwapChain = (MRSTLFB_SWAPCHAIN*)MRSTLFBAllocKernelMem(sizeof(MRSTLFB_SWAPCHAIN));
	if(!psSwapChain)
	{
		return (PVRSRV_ERROR_OUT_OF_MEMORY);
	}

	for(iSCId = 0;iSCId < MAX_SWAPCHAINS;++iSCId) 
	{
		if( psDevInfo->apsSwapChains[iSCId] == NULL )
		{
			psDevInfo->apsSwapChains[iSCId] = psSwapChain;
			break;
		}
	}

	if(iSCId == MAX_SWAPCHAINS) 
	{
		eError = PVRSRV_ERROR_OUT_OF_MEMORY;
		goto ErrorFreeSwapChain;
	}

	ppsBuffer = (MRSTLFB_BUFFER**)MRSTLFBAllocKernelMem(sizeof(MRSTLFB_BUFFER*) * ui32BufferCount);
	if(!ppsBuffer)
	{
		eError = PVRSRV_ERROR_OUT_OF_MEMORY;
		goto ErrorFreeSwapChain;
	}
	for (i = 0; i < ui32BufferCount; i++) ppsBuffer[i] = NULL;
	

	psVSyncFlips = (MRSTLFB_VSYNC_FLIP_ITEM *)MRSTLFBAllocKernelMem(sizeof(MRSTLFB_VSYNC_FLIP_ITEM) * ulSwapChainLength);
	if (!psVSyncFlips)
	{
		eError = PVRSRV_ERROR_OUT_OF_MEMORY;
		goto ErrorFreeBuffers;
	}

	psSwapChain->ulSwapChainLength = ulSwapChainLength;
	psSwapChain->ulBufferCount = (unsigned long)ui32BufferCount;
	psSwapChain->ppsBuffer = ppsBuffer;
	psSwapChain->psVSyncFlips = psVSyncFlips;
	psSwapChain->ulInsertIndex = 0;
	psSwapChain->ulRemoveIndex = 0;
	psSwapChain->psPVRJTable = &psDevInfo->sPVRJTable;

	/* Create bufffers */
	/* Configure the swapchain buffers */
	for (i = 0; i < ui32BufferCount; i++)
	{
		unsigned long bufSize = psDevInfo->sDisplayDim.ui32ByteStride * psDevInfo->sDisplayDim.ui32Height;
		if(MRSTLFBAllocBuffer(psDevInfo, bufSize, &ppsBuffer[i] ) != MRST_OK ) {
			eError = PVRSRV_ERROR_OUT_OF_MEMORY;
			goto ErrorFreeAllocatedBuffes;
		}
		ppsBuffer[i]->psSyncData = ppsSyncData[i];		
	}

	/* Prepare the VSync items */
	for (i = 0; i < ulSwapChainLength; i++)
	{
		psVSyncFlips[i].bValid = MRST_FALSE;
		psVSyncFlips[i].bFlipped = MRST_FALSE;
		psVSyncFlips[i].bCmdCompleted = MRST_FALSE;
	}

	psDrmDev = psDevInfo->psDrmDevice;

	psSwapChain->psDevInfo = psDevInfo;
	psSwapChain->psDrmDev = psDrmDev;
	psSwapChain->psDrmDriver = psDrmDev->driver;

	spin_lock_irqsave(&psDevInfo->sSwapChainLock, ulLockFlags);
   
	psSwapChain->ui32SwapChainID = *pui32SwapChainID = iSCId+1;

	if(psDevInfo->psCurrentSwapChain == NULL)
		psDevInfo->psCurrentSwapChain = psSwapChain;

	psDevInfo->ui32SwapChainNum++;
	if(psDevInfo->ui32SwapChainNum == 1)
	{
		MRSTLFBEnableVSyncInterrupt(psDevInfo);
	}

	spin_unlock_irqrestore(&psDevInfo->sSwapChainLock, ulLockFlags);

	/* Return swapchain handle */
	*phSwapChain = (IMG_HANDLE)psSwapChain;

	return (PVRSRV_OK);

ErrorFreeAllocatedBuffes:
	for (i = 0; i < ui32BufferCount; i++)
	{
		if(ppsBuffer[i] != NULL) 
			MRSTLFBFreeBuffer( psDevInfo, &ppsBuffer[i] );
	}
	MRSTLFBFreeKernelMem(psVSyncFlips);
ErrorFreeBuffers:
	MRSTLFBFreeKernelMem(ppsBuffer);
ErrorFreeSwapChain:
	if(iSCId != MAX_SWAPCHAINS && psDevInfo->apsSwapChains[iSCId] == psSwapChain ) 
		psDevInfo->apsSwapChains[iSCId] = NULL;	
	MRSTLFBFreeKernelMem(psSwapChain);

	return eError;
}

/*
 * DestroyDCSwapChain
 * Called from services.
 */
static PVRSRV_ERROR DestroyDCSwapChain(IMG_HANDLE hDevice,
	IMG_HANDLE hSwapChain)
{
	MRSTLFB_DEVINFO	*psDevInfo;
	MRSTLFB_SWAPCHAIN *psSwapChain;
	unsigned long ulLockFlags;
	int i;

	/* Check parameters */
	if(!hDevice || !hSwapChain)
	{
		return (PVRSRV_ERROR_INVALID_PARAMS);
	}
	
	psDevInfo = (MRSTLFB_DEVINFO*)hDevice;
	psSwapChain = (MRSTLFB_SWAPCHAIN*)hSwapChain;

	spin_lock_irqsave(&psDevInfo->sSwapChainLock, ulLockFlags);

	psDevInfo->ui32SwapChainNum--;

	if(psDevInfo->ui32SwapChainNum == 0) 
	{
		MRSTLFBDisableVSyncInterrupt(psDevInfo);
		psDevInfo->psCurrentSwapChain = NULL;
	}

	psDevInfo->apsSwapChains[ psSwapChain->ui32SwapChainID -1] = NULL;

	/* Flush swap chain */
	FlushInternalVSyncQueue(psSwapChain, psDevInfo->ui32SwapChainNum == 0);

	if (psDevInfo->ui32SwapChainNum == 0)
	{
		/* Flip to primary surface */
		DRMLFBFlipBuffer(psDevInfo, NULL, &psDevInfo->sSystemBuffer);
		MRSTLFBClearSavedFlip(psDevInfo);
	}

	if(psDevInfo->psCurrentSwapChain == psSwapChain)
		psDevInfo->psCurrentSwapChain = NULL;

	spin_unlock_irqrestore(&psDevInfo->sSwapChainLock, ulLockFlags);

	/* Free resources */
	for (i = 0; i < psSwapChain->ulBufferCount; i++)
	{
		MRSTLFBFreeBuffer(psDevInfo, &psSwapChain->ppsBuffer[i] );
	}
	MRSTLFBFreeKernelMem(psSwapChain->psVSyncFlips);
	MRSTLFBFreeKernelMem(psSwapChain->ppsBuffer);
	MRSTLFBFreeKernelMem(psSwapChain);

	return (PVRSRV_OK);
}

/*
 * SetDCDstRect
 * Called from services.
 */
static PVRSRV_ERROR SetDCDstRect(IMG_HANDLE hDevice,
	IMG_HANDLE hSwapChain,
	IMG_RECT *psRect)
{
	UNREFERENCED_PARAMETER(hDevice);
	UNREFERENCED_PARAMETER(hSwapChain);
	UNREFERENCED_PARAMETER(psRect);

	/* Only full display swapchains on this device */
	
	return (PVRSRV_ERROR_NOT_SUPPORTED);
}

/*
 * SetDCSrcRect
 * Called from services.
 */
static PVRSRV_ERROR SetDCSrcRect(IMG_HANDLE hDevice,
                                 IMG_HANDLE hSwapChain,
                                 IMG_RECT *psRect)
{
	UNREFERENCED_PARAMETER(hDevice);
	UNREFERENCED_PARAMETER(hSwapChain);
	UNREFERENCED_PARAMETER(psRect);

	/* Only full display swapchains on this device */

	return (PVRSRV_ERROR_NOT_SUPPORTED);
}

/*
 * SetDCDstColourKey
 * Called from services.
 */
static PVRSRV_ERROR SetDCDstColourKey(IMG_HANDLE hDevice,
                                      IMG_HANDLE hSwapChain,
                                      IMG_UINT32 ui32CKColour)
{
	UNREFERENCED_PARAMETER(hDevice);
	UNREFERENCED_PARAMETER(hSwapChain);
	UNREFERENCED_PARAMETER(ui32CKColour);

	/* Don't support DST CK on this device */

	return (PVRSRV_ERROR_NOT_SUPPORTED);
}

/*
 * SetDCSrcColourKey
 * Called from services.
 */
static PVRSRV_ERROR SetDCSrcColourKey(IMG_HANDLE hDevice,
                                      IMG_HANDLE hSwapChain,
                                      IMG_UINT32 ui32CKColour)
{
	UNREFERENCED_PARAMETER(hDevice);
	UNREFERENCED_PARAMETER(hSwapChain);
	UNREFERENCED_PARAMETER(ui32CKColour);

	/* Don't support SRC CK on this device */

	return (PVRSRV_ERROR_NOT_SUPPORTED);
}

/*
 * GetDCBuffers
 * Called from services.
 */
static PVRSRV_ERROR GetDCBuffers(IMG_HANDLE hDevice,
                                 IMG_HANDLE hSwapChain,
                                 IMG_UINT32 *pui32BufferCount,
                                 IMG_HANDLE *phBuffer)
{
	MRSTLFB_DEVINFO   *psDevInfo;
	MRSTLFB_SWAPCHAIN *psSwapChain;
	unsigned long      i;
	
	/* Check parameters */
	if(!hDevice 
	|| !hSwapChain
	|| !pui32BufferCount
	|| !phBuffer)
	{
		return (PVRSRV_ERROR_INVALID_PARAMS);
	}
	
	psDevInfo = (MRSTLFB_DEVINFO*)hDevice;
	psSwapChain = (MRSTLFB_SWAPCHAIN*)hSwapChain;
	
	/* Return the buffer count */
	*pui32BufferCount = (IMG_UINT32)psSwapChain->ulBufferCount;
	
	/* Return the buffers */
	for (i = 0; i < psSwapChain->ulBufferCount; i++)
	{
		phBuffer[i] = (IMG_HANDLE)psSwapChain->ppsBuffer[i];
	}
	
	return (PVRSRV_OK);
}

/*
 * SwapToDCBuffer
 * Called from services.
 */
static PVRSRV_ERROR SwapToDCBuffer(IMG_HANDLE hDevice,
                                   IMG_HANDLE hBuffer,
                                   IMG_UINT32 ui32SwapInterval,
                                   IMG_HANDLE hPrivateTag,
                                   IMG_UINT32 ui32ClipRectCount,
                                   IMG_RECT *psClipRect)
{
	MRSTLFB_DEVINFO *psDevInfo;

	UNREFERENCED_PARAMETER(ui32SwapInterval);
	UNREFERENCED_PARAMETER(hPrivateTag);
	UNREFERENCED_PARAMETER(psClipRect);
	
	if(!hDevice 
	|| !hBuffer
	|| (ui32ClipRectCount != 0))
	{
		return (PVRSRV_ERROR_INVALID_PARAMS);
	}

	psDevInfo = (MRSTLFB_DEVINFO*)hDevice;

	/*
	 * Nothing to do since services common code does the work in the
	 * general case
	 */

	return (PVRSRV_OK);
}

/************************************************************
	command processing and interrupt specific functions:
************************************************************/
/* VSync interrupt handler function */
static MRST_BOOL MRSTLFBVSyncIHandler(MRSTLFB_DEVINFO *psDevInfo)
{
	MRST_BOOL bStatus = MRST_TRUE;
	MRSTLFB_VSYNC_FLIP_ITEM *psFlipItem;
	unsigned long ulMaxIndex;
	unsigned long ulLockFlags;
	MRSTLFB_SWAPCHAIN *psSwapChain;

	spin_lock_irqsave(&psDevInfo->sSwapChainLock, ulLockFlags);

	/* Is there a swap chain? */
	psSwapChain = psDevInfo->psCurrentSwapChain;
	if (psSwapChain == NULL)
	{
		goto ExitUnlock;
	}

	/*
	 * Don't do anything if a flush is in progress, or if we shouldn't be
	 * displaying anything.
	 */
	if (psDevInfo->bFlushCommands || psDevInfo->bSuspended || psDevInfo->bLeaveVT)
	{
		goto ExitUnlock;
	}

	psFlipItem = &psSwapChain->psVSyncFlips[psSwapChain->ulRemoveIndex];
	ulMaxIndex = psSwapChain->ulSwapChainLength - 1;

	while(psFlipItem->bValid)
	{	
		/* Have we already flipped BEFORE this interrupt? */
		if(psFlipItem->bFlipped)
		{
			/* Have we already called command completed? */
			if(!psFlipItem->bCmdCompleted)
			{
				//FIXME : should we really schedule the MISR in the call below ? Here is what PVRPDP does
				MRST_BOOL bScheduleMISR;
				/* only schedule the MISR if the display vsync is on its own LISR */
				bScheduleMISR = MRST_TRUE;

				/* Call command complete for the flip */
				psSwapChain->psPVRJTable->pfnPVRSRVCmdComplete((IMG_HANDLE)psFlipItem->hCmdComplete, bScheduleMISR);

				/* Signal we've done the command complete */
				psFlipItem->bCmdCompleted = MRST_TRUE;
			}

			/* We've cmd completed so decrement the swap interval */
			psFlipItem->ulSwapInterval--;

			/* Can we remove the flip item? */
			if(psFlipItem->ulSwapInterval == 0)
			{	
				/* Advance the remove index */
				psSwapChain->ulRemoveIndex++;
				
				if(psSwapChain->ulRemoveIndex > ulMaxIndex)
				{
					psSwapChain->ulRemoveIndex = 0;
				}
				
				/* Clear item state */
				psFlipItem->bCmdCompleted = MRST_FALSE;
				psFlipItem->bFlipped = MRST_FALSE;
	
				/*
				 * Only mark as invalid once item data is
				 * finished with
				 */
				psFlipItem->bValid = MRST_FALSE;
			}
			else
			{
				/* we're waiting for the last flip to finish
				 * displaying so the remove index hasn't been
				 * updated to block any new flips occuring.
				 * Nothing more to do on interrupt
				*/
				break;
			}
		}
		else
		{
			/* Flip to new surface */
			DRMLFBFlipBuffer(psDevInfo, psSwapChain, psFlipItem->psBuffer);
			
			/* Indicate we've issued the flip */
			psFlipItem->bFlipped = MRST_TRUE;
			
			/* Nothing more to do for this interrupt */
			break;
		}
		
		/* Update to next flip item */
		psFlipItem = &psSwapChain->psVSyncFlips[psSwapChain->ulRemoveIndex];
	}
		
ExitUnlock:
	spin_unlock_irqrestore(&psDevInfo->sSwapChainLock, ulLockFlags);

	return bStatus;
}



#if defined(MRST_USING_INTERRUPTS)
/* Interrupt service routine  */
static int
MRSTLFBVSyncISR(struct drm_device *psDrmDevice, int iPipe)
{
	MRSTLFB_DEVINFO *psDevInfo = GetAnchorPtr();	

	MRSTLFBVSyncIHandler(psDevInfo);

 	/* The return result is ignored */
	return 0;
}
#endif

/* Command processing flip handler function.  Called from services. */
static IMG_BOOL ProcessFlip(IMG_HANDLE  hCmdCookie,
                            IMG_UINT32  ui32DataSize,
                            IMG_VOID   *pvData)
{
	DISPLAYCLASS_FLIP_COMMAND *psFlipCmd;
	MRSTLFB_DEVINFO *psDevInfo;
	MRSTLFB_BUFFER *psBuffer;
	MRSTLFB_SWAPCHAIN *psSwapChain;
#if defined(MRST_USING_INTERRUPTS)
	MRSTLFB_VSYNC_FLIP_ITEM* psFlipItem;
#endif
	unsigned long ulLockFlags;

	/* Check parameters  */
	if(!hCmdCookie || !pvData)
	{
		return IMG_FALSE;
	}

	/* Validate data packet  */
	psFlipCmd = (DISPLAYCLASS_FLIP_COMMAND*)pvData;

	if (psFlipCmd == IMG_NULL || sizeof(DISPLAYCLASS_FLIP_COMMAND) != ui32DataSize)
	{
		return IMG_FALSE;
	}

	/* Setup some useful pointers  */
	psDevInfo = (MRSTLFB_DEVINFO*)psFlipCmd->hExtDevice;
	/* The buffer we are flipping to */
	psBuffer = (MRSTLFB_BUFFER*)psFlipCmd->hExtBuffer;
	psSwapChain = (MRSTLFB_SWAPCHAIN*) psFlipCmd->hExtSwapChain;

	spin_lock_irqsave(&psDevInfo->sSwapChainLock, ulLockFlags);

#if defined(MRST_USING_INTERRUPTS)
	/*
	 * If swap interval is 0, or the swap chain is being flushed,
	 * process the flip immediately.
	 */

	if(psFlipCmd->ui32SwapInterval == 0 || psDevInfo->bFlushCommands)
	{
#endif
		DRMLFBFlipBuffer(psDevInfo, psSwapChain, psBuffer);

		/* Call command complete callback  */
	// FIXME : same MISR question (see above)
		psSwapChain->psPVRJTable->pfnPVRSRVCmdComplete(hCmdCookie, IMG_TRUE);

#if defined(MRST_USING_INTERRUPTS)
		goto ExitTrueUnlock;
	}

	psFlipItem = &psSwapChain->psVSyncFlips[psSwapChain->ulInsertIndex];

	/* Try to insert command into list */
	if(psFlipItem->bValid == MRST_FALSE)
	{
		unsigned long ulMaxIndex = psSwapChain->ulSwapChainLength - 1;
		
		if(psSwapChain->ulInsertIndex == psSwapChain->ulRemoveIndex)
		{
			/* Flip to new surface  */
			DRMLFBFlipBuffer(psDevInfo, psSwapChain, psBuffer);

			psFlipItem->bFlipped = MRST_TRUE;
		}
		else
		{
			psFlipItem->bFlipped = MRST_FALSE;
		}

		psFlipItem->hCmdComplete = (MRST_HANDLE)hCmdCookie;
		psFlipItem->ulSwapInterval = (unsigned long)psFlipCmd->ui32SwapInterval;
		psFlipItem->psBuffer = psBuffer;
		psFlipItem->bValid = MRST_TRUE;

		psSwapChain->ulInsertIndex++;
		if(psSwapChain->ulInsertIndex > ulMaxIndex)
		{
			psSwapChain->ulInsertIndex = 0;
		}

		goto ExitTrueUnlock;
	}
	
	spin_unlock_irqrestore(&psDevInfo->sSwapChainLock, ulLockFlags);
	return IMG_FALSE;

ExitTrueUnlock:
#endif
	spin_unlock_irqrestore(&psDevInfo->sSwapChainLock, ulLockFlags);
	return IMG_TRUE;
}


#if defined(PVR_MRST_FB_SET_PAR_ON_INIT)
/*
 * If the driver is built as a separate module, the console driver may
 * not have grabbed the framebuffer created by this driver, by the time
 * the X Server initialisation is under way.  As a result, fb_set_par
 * doesn't get called in time, resulting in some DRM initialistion not
 * being done.  This results in X Server hardware cursor initialisation
 * failing, although the framebuffer is otherwise usable.
 */
static void MRSTFBSetPar(struct fb_info *psLINFBInfo)
{
	acquire_console_sem();

	if (psLINFBInfo->fbops->fb_set_par != NULL)
	{
		int res;

		res = psLINFBInfo->fbops->fb_set_par(psLINFBInfo);
		if (res != 0)
		{
			printk(KERN_WARNING DRIVER_PREFIX
				": fb_set_par failed: %d\n", res);

		}
	}
	else
	{
		printk(KERN_WARNING DRIVER_PREFIX
			": fb_set_par not set - HW cursor may not work\n");
	}

	release_console_sem();
}
#endif

void MRSTLFBSuspend(void)
{
	MRSTLFB_DEVINFO *psDevInfo = GetAnchorPtr();
	unsigned long ulLockFlags;

	spin_lock_irqsave(&psDevInfo->sSwapChainLock, ulLockFlags);

	if (!psDevInfo->bSuspended)
	{
#if !defined(PVR_MRST_STYLE_PM)
		if(psDevInfo->ui32SwapChainNum != 0)
		{
			MRSTLFBDisableVSyncInterrupt(psDevInfo);
		}
#endif
		psDevInfo->bSuspended = MRST_TRUE;
	}

	spin_unlock_irqrestore(&psDevInfo->sSwapChainLock, ulLockFlags);
}

void MRSTLFBResume(void)
{
	MRSTLFB_DEVINFO *psDevInfo = GetAnchorPtr();
	unsigned long ulLockFlags;

	spin_lock_irqsave(&psDevInfo->sSwapChainLock, ulLockFlags);

	if (psDevInfo->bSuspended)
	{
#if !defined(PVR_MRST_STYLE_PM)
		if(psDevInfo->ui32SwapChainNum != 0)
		{
			MRSTLFBEnableVSyncInterrupt(psDevInfo);
		}
#endif
		psDevInfo->bSuspended = MRST_FALSE;

		MRSTLFBRestoreLastFlip(psDevInfo);
	}

	spin_unlock_irqrestore(&psDevInfo->sSwapChainLock, ulLockFlags);

#if !defined(PVR_MRST_STYLE_PM)
	(void) UnblankDisplay(psDevInfo);
#endif
}

/*!
******************************************************************************

 @Function	InitDev
 
 @Description specifies devices in the systems memory map
 
 @Input    psSysData - sys data

 @Return   MRST_ERROR  :

******************************************************************************/

/* Use intel version of framebuffer - it uses our PVRSRV_KERNEL_MEMINFO for that */
#ifdef DRM_PVR_USE_INTEL_FB
#include "mm.h"
int MRSTLFBHandleChangeFB(struct drm_device* dev, struct psb_framebuffer *psbfb)
{
	MRSTLFB_DEVINFO *psDevInfo = GetAnchorPtr();
	int i;
	struct drm_psb_private * dev_priv;
	struct psb_gtt * pg;
	
	if( !psDevInfo->sSystemBuffer.bIsContiguous )
		MRSTLFBFreeKernelMem( psDevInfo->sSystemBuffer.uSysAddr.psNonCont );
	
	dev_priv = (struct drm_psb_private *)dev->dev_private;
	pg = dev_priv->pg;
	
	psDevInfo->sDisplayDim.ui32ByteStride = psbfb->base.pitches[0];
	psDevInfo->sDisplayDim.ui32Width = psbfb->base.width;
	psDevInfo->sDisplayDim.ui32Height = psbfb->base.height;

	psDevInfo->sSystemBuffer.ui32BufferSize = psbfb->size;
	//psDevInfo->sSystemBuffer.sCPUVAddr = psbfb->pvKMAddr;
	psDevInfo->sSystemBuffer.sCPUVAddr = pg->vram_addr;
	//psDevInfo->sSystemBuffer.sDevVAddr.uiAddr = psbfb->offsetGTT;
	psDevInfo->sSystemBuffer.sDevVAddr.uiAddr = 0;
	psDevInfo->sSystemBuffer.bIsAllocated = IMG_FALSE;	

	if(psbfb->bo ) 
	{
		
		psDevInfo->sSystemBuffer.bIsContiguous = IMG_FALSE;
		psDevInfo->sSystemBuffer.uSysAddr.psNonCont = MRSTLFBAllocKernelMem( sizeof( IMG_SYS_PHYADDR ) * psbfb->bo->ttm->num_pages);	
		for(i = 0;i < psbfb->bo->ttm->num_pages;++i) 
		{
			struct page *p = ttm_tt_get_page( psbfb->bo->ttm, i);
			psDevInfo->sSystemBuffer.uSysAddr.psNonCont[i].uiAddr = page_to_pfn(p) << PAGE_SHIFT;   
			
		}
	} 
	else 
	{
		
		//struct drm_device * psDrmDevice = psDevInfo->psDrmDevice;	
		//struct drm_psb_private * dev_priv = (struct drm_psb_private *)psDrmDevice->dev_private;
		//struct psb_gtt * pg = dev_priv->pg;

		psDevInfo->sSystemBuffer.bIsContiguous = IMG_TRUE;
		psDevInfo->sSystemBuffer.uSysAddr.sCont.uiAddr = pg->stolen_base;
	}

	return 0;
}
#else

/* Use pvr version of framebuffer - it uses simple vmalloc or ioremap */
static int MRSTLFBHandleChangeFB(struct drm_device* dev, struct psb_framebuffer *psbfb)
{
	MRSTLFB_DEVINFO *psDevInfo = GetAnchorPtr();
	int i;

	/** Clean up after previous non contiguous system buffer */
	if( !psDevInfo->sSystemBuffer.bIsContiguous )
		MRSTLFBFreeKernelMem( psDevInfo->sSystemBuffer.uSysAddr.psNonCont );

	/* Update system buffer */
	psDevInfo->sDisplayDim.ui32ByteStride = psbfb->base.pitch;
	psDevInfo->sDisplayDim.ui32Width = psbfb->base.width;
	psDevInfo->sDisplayDim.ui32Height = psbfb->base.height;

	psDevInfo->sSystemBuffer.ui32BufferSize = psbfb->buf.size;
	psDevInfo->sSystemBuffer.sCPUVAddr = psbfb->buf.kMapping;
	psDevInfo->sSystemBuffer.sDevVAddr.uiAddr = psbfb->buf.offsetGTT;
	psDevInfo->sSystemBuffer.bIsAllocated = MRST_FALSE;	

	if ( psbfb->buf.type == PSB_BUFFER_VRAM )
	{
		/* System frame buffer is stolen memory hole. Get stolen base address. */
		struct drm_device * psDrmDevice = psDevInfo->psDrmDevice;	
		struct drm_psb_private * dev_priv = (struct drm_psb_private *)psDrmDevice->dev_private;
		struct psb_gtt * pg = dev_priv->pg;

		psDevInfo->sSystemBuffer.bIsContiguous = MRST_TRUE;
		psDevInfo->sSystemBuffer.uSysAddr.sCont.uiAddr = pg->stolen_base;
	} else {
		/* System frame buffer is TTM object buffer. Retrieve physical frame addresses. */
		psDevInfo->sSystemBuffer.bIsContiguous = MRST_FALSE;
		psDevInfo->sSystemBuffer.uSysAddr.psNonCont = MRSTLFBAllocKernelMem( sizeof( IMG_SYS_PHYADDR ) * (psbfb->buf.pagesNum));	
		for (i = 0; i < psbfb->buf.pagesNum; i++) 
		{
			psDevInfo->sSystemBuffer.uSysAddr.psNonCont[i].uiAddr = psbfb_get_buffer_pfn( psDevInfo->psDrmDevice, &psbfb->buf, i) << PAGE_SHIFT;   
		}
	} 

	return 0;
}
#endif

MRST_ERROR MRSTLFBChangeSwapChainProperty(unsigned long *psSwapChainGTTOffset,
		unsigned long ulSwapChainGTTSize, IMG_INT32 i32Pipe)
{
	MRSTLFB_DEVINFO *psDevInfo = GetAnchorPtr();
	struct drm_device *psDrmDevice = NULL;
	struct drm_psb_private *dev_priv = NULL;
	struct psb_gtt *pg = NULL;
	uint32_t tt_pages = 0;
	uint32_t max_gtt_offset = 0;

	int iSwapChainAttachedPlane = PVRSRV_SWAPCHAIN_ATTACHED_PLANE_NONE;
	IMG_UINT32 ui32SwapChainID = 0;
	unsigned long ulLockFlags;
	unsigned long ulCurrentSwapChainGTTOffset = 0;
	MRST_ERROR eError = MRST_ERROR_GENERIC;

	if (psDevInfo == IMG_NULL) {
		DRM_DEBUG("MRSTLFB hasn't been initialized\n");
		/* Won't attach/de-attach the plane in case of no swap chain
		 * created. */
		eError = MRST_ERROR_INIT_FAILURE;
		return eError;
	}

	if (psDevInfo->apsSwapChains == IMG_NULL) {
		DRM_ERROR("No swap chain.\n");
		return eError;
	}

	psDrmDevice = psDevInfo->psDrmDevice;
	dev_priv = (struct drm_psb_private *)psDrmDevice->dev_private;
	pg = dev_priv->pg;
	if (pg == NULL) {
		DRM_ERROR("Invalid GTT data.\n");
		return eError;
	}

	tt_pages = (pg->gatt_pages < PSB_TT_PRIV0_PLIMIT) ?
		(pg->gatt_pages) : PSB_TT_PRIV0_PLIMIT;

	/* Another half of GTT is managed by TTM. */
	tt_pages /= 2;
	max_gtt_offset = tt_pages << PAGE_SHIFT;

	if ((psSwapChainGTTOffset == IMG_NULL) ||
			(ulSwapChainGTTSize == 0) ||
			((*psSwapChainGTTOffset + ulSwapChainGTTSize) >
			 max_gtt_offset)) {
		DRM_ERROR("Invalid GTT offset.\n");
		return eError;
	}

	switch (i32Pipe) {
	case 0:
		iSwapChainAttachedPlane = PVRSRV_SWAPCHAIN_ATTACHED_PLANE_A;
		break;
	case 1:
		iSwapChainAttachedPlane = PVRSRV_SWAPCHAIN_ATTACHED_PLANE_B;
		break;
	case 2:
		iSwapChainAttachedPlane = PVRSRV_SWAPCHAIN_ATTACHED_PLANE_C;
		break;
	default:
		DRM_ERROR("Illegal Pipe Number.\n");
		return eError;
	}

	spin_lock_irqsave(&psDevInfo->sSwapChainLock, ulLockFlags);

	ulCurrentSwapChainGTTOffset =
		psDevInfo->apsSwapChains[ui32SwapChainID]->ulSwapChainGTTOffset;

	for (ui32SwapChainID = 0; ui32SwapChainID < psDevInfo->ui32SwapChainNum;
			ui32SwapChainID++) {
		if (psDevInfo->apsSwapChains[ui32SwapChainID] == IMG_NULL)
			continue;

		if (*psSwapChainGTTOffset == ulCurrentSwapChainGTTOffset) {
			psDevInfo->apsSwapChains[ui32SwapChainID]->ui32SwapChainPropertyFlag |= iSwapChainAttachedPlane;

			/*
			 * Trigger the display plane to flip to the swap
			 * chain's last flip surface, to avoid that it still
			 * displays with original GTT offset after mode setting
			 * and attached to the specific swap chain.
			 */
			if (psDevInfo->bLastFlipAddrValid)
				*psSwapChainGTTOffset =
					psDevInfo->ulLastFlipAddr;
		}
		else
			psDevInfo->apsSwapChains[ui32SwapChainID]->ui32SwapChainPropertyFlag &= ~iSwapChainAttachedPlane;
	}

	spin_unlock_irqrestore(&psDevInfo->sSwapChainLock, ulLockFlags);

	eError = MRST_OK;
	return eError;
}

static int MRSTLFBFindMainPipe(struct drm_device *dev)  
{
	struct drm_crtc *crtc;

	list_for_each_entry(crtc, &dev->mode_config.crtc_list, head)  
	{
		if ( drm_helper_crtc_in_use(crtc) ) 
		{
			struct psb_intel_crtc *psb_intel_crtc = to_psb_intel_crtc(crtc);
			return psb_intel_crtc->pipe;
		}
	}	
	// Default to 0
	return 0;
}

static int DRMLFBLeaveVTHandler(struct drm_device *dev)  
{
	MRSTLFB_DEVINFO *psDevInfo = GetAnchorPtr();
  	unsigned long ulLockFlags;
 
	spin_lock_irqsave(&psDevInfo->sSwapChainLock, ulLockFlags);

	if (!psDevInfo->bLeaveVT)
	{
		if(psDevInfo->psCurrentSwapChain != NULL)
		{
			FlushInternalVSyncQueue(psDevInfo->psCurrentSwapChain, MRST_TRUE);
			SetFlushStateNoLock(psDevInfo, MRST_TRUE);
		}

		DRMLFBFlipBuffer(psDevInfo, NULL, &psDevInfo->sSystemBuffer);

		psDevInfo->bLeaveVT = MRST_TRUE;
	}

	spin_unlock_irqrestore(&psDevInfo->sSwapChainLock, ulLockFlags);

	return 0;
}

static int DRMLFBEnterVTHandler(struct drm_device *dev)  
{
	MRSTLFB_DEVINFO *psDevInfo = GetAnchorPtr();
  	unsigned long ulLockFlags;

	spin_lock_irqsave(&psDevInfo->sSwapChainLock, ulLockFlags);

	if (psDevInfo->bLeaveVT)
	{
		if(psDevInfo->psCurrentSwapChain != NULL)
		{
			SetFlushStateNoLock(psDevInfo, MRST_FALSE);
		}

		psDevInfo->bLeaveVT = MRST_FALSE;

		MRSTLFBRestoreLastFlip(psDevInfo);
	}

	spin_unlock_irqrestore(&psDevInfo->sSwapChainLock, ulLockFlags);

	return 0;
}

/*!
******************************************************************************

 @Function	InitDev
 
 @Description specifies devices in the systems memory map
 
 @Input    psSysData - sys data

 @Return   MRST_ERROR  :

******************************************************************************/
static MRST_ERROR InitDev(MRSTLFB_DEVINFO *psDevInfo)
{
	MRST_ERROR eError = MRST_ERROR_GENERIC;
	struct fb_info *psLINFBInfo;
	struct drm_device * psDrmDevice = psDevInfo->psDrmDevice;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,35))
	struct drm_psb_private * psDrmPrivate = (struct drm_psb_private *)psDrmDevice->dev_private;
	struct psb_fbdev * psPsbFBDev = (struct psb_fbdev *)psDrmPrivate->fbdev;
#endif
	struct drm_framebuffer * psDrmFB;
	struct psb_framebuffer *psbfb;
	/*	struct drm_display_mode * panel_fixed_mode = mode_dev->panel_fixed_mode;
	int hdisplay = panel_fixed_mode->hdisplay;
	int vdisplay = panel_fixed_mode->vdisplay;*/
	int hdisplay;// = panel_fixed_mode->hdisplay;
	int vdisplay;// = panel_fixed_mode->vdisplay;
	int i;
	unsigned long FBSize;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,35))
	psDrmFB = psPsbFBDev->psb_fb_helper.fb;
#else
	psDrmFB = list_first_entry(&psDrmDevice->mode_config.fb_kernel_list, 
				   struct drm_framebuffer, 
				   filp_head);
#endif
	if(!psDrmFB) {
		printk(KERN_INFO"%s: Cannot find drm FB",__FUNCTION__);
		return eError;
	}
	psbfb = to_psb_fb(psDrmFB);

	hdisplay = psDrmFB->width;
	vdisplay = psDrmFB->height;
	FBSize = psDrmFB->pitches[0] * psDrmFB->height;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,35))
	psLINFBInfo = (struct fb_info*)psPsbFBDev->psb_fb_helper.fbdev;
#else
	psLINFBInfo = (struct fb_info*)psDrmFB->fbdev;	
#endif

#if defined(PVR_MRST_FB_SET_PAR_ON_INIT)
	MRSTFBSetPar(psLINFBInfo);
#endif

	
	psDevInfo->sSystemBuffer.bIsContiguous = MRST_TRUE;
	psDevInfo->sSystemBuffer.bIsAllocated = MRST_FALSE;

	MRSTLFBHandleChangeFB(psDrmDevice, psbfb);

	switch( psDrmFB->depth ) 
	{
	case 32:
	case 24:
		{
			psDevInfo->sDisplayFormat.pixelformat = PVRSRV_PIXEL_FORMAT_ARGB8888;
			break;
		}
	case 16:
		{
			psDevInfo->sDisplayFormat.pixelformat = PVRSRV_PIXEL_FORMAT_RGB565;
			break;
		}	
	default:
		{			
			printk(KERN_ERR"%s: Unknown bit depth %d\n",__FUNCTION__,psDrmFB->depth);
		}
	}
	psDevInfo->psLINFBInfo = psLINFBInfo;

	psDevInfo->ui32MainPipe = MRSTLFBFindMainPipe(psDevInfo->psDrmDevice);
	
	for(i = 0;i < MAX_SWAPCHAINS;++i) 
	{
		psDevInfo->apsSwapChains[i] = NULL;
	}

	/* Map the registers needed for flipping the display */
	/* The ioremap below was done in the Tungsten Moorestown Driver. Here we just retrieve the address */
	// psSwapChain->pvRegs = ioremap(psDevInfo->psLINFBInfo->fix.mmio_start, psDevInfo->psLINFBInfo->fix.mmio_len);
	psDevInfo->pvRegs = psbfb_vdc_reg(psDevInfo->psDrmDevice);

	if (psDevInfo->pvRegs == NULL)
	{
		eError = PVRSRV_ERROR_BAD_MAPPING;
		printk(KERN_WARNING DRIVER_PREFIX ": Couldn't map registers needed for flipping\n");
		return eError;
	}

	return MRST_OK;
}

MRST_ERROR MRSTLFBInit(struct drm_device * dev)
{

	MRSTLFB_DEVINFO		*psDevInfo;
	struct drm_psb_private *psDrmPriv = (struct drm_psb_private *)dev->dev_private;

	psDevInfo = GetAnchorPtr();
	
	if (psDevInfo == NULL)
	{
		PFN_CMD_PROC	 		pfnCmdProcList[MRSTLFB_COMMAND_COUNT];
		IMG_UINT32				aui32SyncCountList[MRSTLFB_COMMAND_COUNT][2];
		/* Allocate device info. structure */
		psDevInfo = (MRSTLFB_DEVINFO *)MRSTLFBAllocKernelMem(sizeof(MRSTLFB_DEVINFO));

		if(!psDevInfo)
		{
			return (MRST_ERROR_OUT_OF_MEMORY);
		}

		/* Any fields not set will be zero */
		memset(psDevInfo, 0, sizeof(MRSTLFB_DEVINFO));

		/* Set the top-level anchor */
		SetAnchorPtr((void*)psDevInfo);

		psDevInfo->psDrmDevice = dev;
		psDevInfo->ulRefCount = 0;

		/* Save private fbdev information structure in the dev. info. */
		if(InitDev(psDevInfo) != MRST_OK)
		{
			return (MRST_ERROR_INIT_FAILURE);
		}

		if(MRSTLFBGetLibFuncAddr ("PVRGetDisplayClassJTable", &pfnGetPVRJTable) != MRST_OK)
		{
			return (MRST_ERROR_INIT_FAILURE);
		}

		/* Got the kernel services function table */
		if(!(*pfnGetPVRJTable)(&psDevInfo->sPVRJTable))
		{
			return (MRST_ERROR_INIT_FAILURE);
		}

		/* Setup the devinfo */		
		spin_lock_init(&psDevInfo->sSwapChainLock);

		psDevInfo->psCurrentSwapChain = NULL;
		psDevInfo->bFlushCommands = MRST_FALSE;

		psDevInfo->sDisplayInfo.ui32MaxSwapChainBuffers = 4;
		psDevInfo->sDisplayInfo.ui32MaxSwapChains = MAX_SWAPCHAINS;
		psDevInfo->sDisplayInfo.ui32MaxSwapInterval = 3;
		psDevInfo->sDisplayInfo.ui32MinSwapInterval = 0;

		strncpy(psDevInfo->sDisplayInfo.szDisplayName, DISPLAY_DEVICE_NAME, MAX_DISPLAY_NAME_SIZE);

		
	

		DEBUG_PRINTK((KERN_INFO DRIVER_PREFIX
			": Maximum number of swap chain buffers: %u\n",
			psDevInfo->sDisplayInfo.ui32MaxSwapChainBuffers));

		/* Setup system buffer */

		/*
			Setup the DC Jtable so SRVKM can call into this driver
		*/
		psDevInfo->sDCJTable.ui32TableSize = sizeof(PVRSRV_DC_SRV2DISP_KMJTABLE);
		psDevInfo->sDCJTable.pfnOpenDCDevice = OpenDCDevice;
		psDevInfo->sDCJTable.pfnCloseDCDevice = CloseDCDevice;
		psDevInfo->sDCJTable.pfnEnumDCFormats = EnumDCFormats;
		psDevInfo->sDCJTable.pfnEnumDCDims = EnumDCDims;
		psDevInfo->sDCJTable.pfnGetDCSystemBuffer = GetDCSystemBuffer;
		psDevInfo->sDCJTable.pfnGetDCInfo = GetDCInfo;
		psDevInfo->sDCJTable.pfnGetBufferAddr = GetDCBufferAddr;
		psDevInfo->sDCJTable.pfnCreateDCSwapChain = CreateDCSwapChain;
		psDevInfo->sDCJTable.pfnDestroyDCSwapChain = DestroyDCSwapChain;
		psDevInfo->sDCJTable.pfnSetDCDstRect = SetDCDstRect;
		psDevInfo->sDCJTable.pfnSetDCSrcRect = SetDCSrcRect;
		psDevInfo->sDCJTable.pfnSetDCDstColourKey = SetDCDstColourKey;
		psDevInfo->sDCJTable.pfnSetDCSrcColourKey = SetDCSrcColourKey;
		psDevInfo->sDCJTable.pfnGetDCBuffers = GetDCBuffers;
		psDevInfo->sDCJTable.pfnSwapToDCBuffer = SwapToDCBuffer;
		psDevInfo->sDCJTable.pfnSetDCState = SetDCState;

		/* Register device with services and retrieve device index */
		if(psDevInfo->sPVRJTable.pfnPVRSRVRegisterDCDevice (
			&psDevInfo->sDCJTable,
			&psDevInfo->uiDeviceID ) != PVRSRV_OK)
		{
			return (MRST_ERROR_DEVICE_REGISTER_FAILED);
		}

		printk("Device ID: %d\n", (int)psDevInfo->uiDeviceID);

		/*
			 - Install an ISR for the Moorestown Vsync interrupt
			 - Disable the ISR on installation
			 - create/destroy swapchain enables/disables the ISR
		*/
		/*
			display hardware may have its own interrupt or can share
			with other devices (device vs. services ISRs).
			where PVR_DEVICE_ISR == device isr type

			Moorestown hardware uses a shared interrupt (PVR_DEVICE_ISR undefined)
		*/


#if defined (MRST_USING_INTERRUPTS)
	/* install a device specific ISR handler  */
	if(MRSTLFBInstallVSyncISR(psDevInfo,MRSTLFBVSyncISR) != MRST_OK)
	{
		DEBUG_PRINTK((KERN_INFO DRIVER_PREFIX	"ISR Installation failed\n"));
		return (MRST_ERROR_INIT_FAILURE);
	}
#endif

	/* Setup private command processing function table ... */
	pfnCmdProcList[DC_FLIP_COMMAND] = ProcessFlip;
	
	/* ... and associated sync count(s) */
	aui32SyncCountList[DC_FLIP_COMMAND][0] = 0; /* no writes */
	aui32SyncCountList[DC_FLIP_COMMAND][1] = 2; /* two reads */
	
	/*
	  Register private command processing functions with
	  the Command Queue Manager and setup the general
	  command complete function in the devinfo.
	*/
	if (psDevInfo->sPVRJTable.pfnPVRSRVRegisterCmdProcList (psDevInfo->uiDeviceID,
								&pfnCmdProcList[0],
								aui32SyncCountList,
								MRSTLFB_COMMAND_COUNT) != PVRSRV_OK)
	  {
	    printk(KERN_WARNING DRIVER_PREFIX ": Can't register callback\n");
	    return (MRST_ERROR_CANT_REGISTER_CALLBACK);
	  }


	}

	/* Set handler for framebuffer change(resize) */
#ifndef DRM_PVR_USE_INTEL_FB
	psDrmPriv->psb_change_fb_handler = MRSTLFBHandleChangeFB;	

	psDrmPriv->psb_leave_vt_handler = DRMLFBLeaveVTHandler;
	psDrmPriv->psb_enter_vt_handler = DRMLFBEnterVTHandler;
#endif

	/* Increment the ref count */
	psDevInfo->ulRefCount++;

	/* Return success */
	return (MRST_OK);	
}

/*
 *	MRSTLFBDeinit
 *	Deinitialises the display class device component of the FBDev
 */
MRST_ERROR MRSTLFBDeinit(void)
{
	MRSTLFB_DEVINFO *psDevInfo, *psDevFirst;

	psDevFirst = GetAnchorPtr();
	psDevInfo = psDevFirst;

	/* Check DevInfo has been setup */
	if (psDevInfo == NULL)
	{
		return (MRST_ERROR_GENERIC);
	}

	/* Decrement ref count */
	psDevInfo->ulRefCount--;

	if (psDevInfo->ulRefCount == 0)
	{
		/* All references gone - de-init device information */
		PVRSRV_DC_DISP2SRV_KMJTABLE	*psJTable = &psDevInfo->sPVRJTable;

		if (psDevInfo->sPVRJTable.pfnPVRSRVRemoveCmdProcList (psDevInfo->uiDeviceID, MRSTLFB_COMMAND_COUNT) != PVRSRV_OK)
		{
			return (MRST_ERROR_GENERIC);
		}

#if defined (MRST_USING_INTERRUPTS)
		/* uninstall device specific ISR handler for PDP */
		if(MRSTLFBUninstallVSyncISR(psDevInfo) != MRST_OK)
		{
			return (MRST_ERROR_GENERIC);
		}
#endif

		/*
		 * Remove display class device from kernel services device
		 * register.
		 */
		if (psJTable->pfnPVRSRVRemoveDCDevice(psDevInfo->uiDeviceID) != PVRSRV_OK)
		{
			return (MRST_ERROR_GENERIC);
		}
		
		/* De-allocate data structure */
		MRSTLFBFreeKernelMem(psDevInfo);
	}
	
	/* Clear the top-level anchor */
	SetAnchorPtr(NULL);

	/* Return success */
	return (MRST_OK);
}



/***********************************************************************************
 Function Name		: MRSTLFBAllocBuffer
 Description		: Allocated VMalloc / Write combine memory and GTT map.
************************************************************************************/
MRST_ERROR MRSTLFBAllocBuffer(struct MRSTLFB_DEVINFO_TAG *psDevInfo, IMG_UINT32 ui32Size, MRSTLFB_BUFFER **ppBuffer)
{
	IMG_VOID *pvBuf;
	IMG_UINT32 ulPagesNumber;
	IMG_UINT32 ulCounter;
	int i;

	pvBuf = __vmalloc( ui32Size, GFP_KERNEL | __GFP_HIGHMEM, __pgprot((pgprot_val(PAGE_KERNEL ) & ~_PAGE_CACHE_MASK) | _PAGE_CACHE_WC) );
	if( pvBuf == NULL ) 
	{
		return MRST_ERROR_OUT_OF_MEMORY;
	}

	ulPagesNumber = (ui32Size + PAGE_SIZE -1) / PAGE_SIZE;

	*ppBuffer = MRSTLFBAllocKernelMem( sizeof( MRSTLFB_BUFFER ) );	
	(*ppBuffer)->sCPUVAddr = pvBuf;
	(*ppBuffer)->ui32BufferSize = ui32Size;
	(*ppBuffer)->uSysAddr.psNonCont = MRSTLFBAllocKernelMem( sizeof( IMG_SYS_PHYADDR ) * ulPagesNumber);	
	(*ppBuffer)->bIsAllocated = MRST_TRUE;
	(*ppBuffer)->bIsContiguous = MRST_FALSE;
	(*ppBuffer)->ui32OwnerTaskID = task_tgid_nr(current);

	i = 0;
	for (ulCounter = 0; ulCounter < ui32Size; ulCounter += PAGE_SIZE) 
	{
		(*ppBuffer)->uSysAddr.psNonCont[i++].uiAddr = vmalloc_to_pfn( pvBuf + ulCounter ) << PAGE_SHIFT;
	}
	
	psb_gtt_map_pvr_memory( psDevInfo->psDrmDevice, 
							(unsigned int)*ppBuffer, 
							(*ppBuffer)->ui32OwnerTaskID,
							(IMG_CPU_PHYADDR*) (*ppBuffer)->uSysAddr.psNonCont, 
							ulPagesNumber,
							&(*ppBuffer)->sDevVAddr.uiAddr, 1 );

	(*ppBuffer)->sDevVAddr.uiAddr <<= PAGE_SHIFT;

   	return MRST_OK;	
}

/***********************************************************************************
 Function Name		: MRSTLFBFreeBuffer
 Description		: Free memory allocated by MRSTLFBAllocBuffer.  (*ppBuffer = 0).
************************************************************************************/
MRST_ERROR MRSTLFBFreeBuffer(struct MRSTLFB_DEVINFO_TAG *psDevInfo, MRSTLFB_BUFFER **ppBuffer)
{
	if( !(*ppBuffer)->bIsAllocated )
		return MRST_ERROR_INVALID_PARAMS;

#ifndef DRM_PVR_USE_INTEL_FB	
	psb_gtt_unmap_memory( psDevInfo->psDrmDevice, 
#else
	psb_gtt_unmap_pvr_memory( psDevInfo->psDrmDevice, 
#endif
							  (unsigned int)*ppBuffer,
							  (*ppBuffer)->ui32OwnerTaskID);

	vfree( (*ppBuffer)->sCPUVAddr );

	MRSTLFBFreeKernelMem( (*ppBuffer)->uSysAddr.psNonCont );
	
	MRSTLFBFreeKernelMem( *ppBuffer);

	*ppBuffer = NULL;

	return MRST_OK;	
}




/******************************************************************************
 End of file (mrstlfb_displayclass.c)
******************************************************************************/

