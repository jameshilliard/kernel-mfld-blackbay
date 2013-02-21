/*************************************************************************/ /*!
@Title          Exports from PVR Services to the rest of the DRM module
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@Description    Interfaces exported from PVR Services to the rest of the
                driver.
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

#include <drm/drmP.h>
#include <drm/drm.h>

#include "pvr_drm_shared.h"

#include "services_headers.h"
#include "private_data.h"
#include "pvr_drm.h"

#include "pvr_bridge.h"
#include "linkage.h"
#include "mmap.h"

#include "drmlfb.h"

#if defined(PDUMP)
#include "linuxsrv.h"
#endif

#include "sys_pvr_drm_import.h"

#include "sys_pvr_drm_export.h"

int
SYSPVRInit(void)
{
	PVRDPFInit();

	return 0;
}


int
SYSPVRLoad(struct drm_device *dev, unsigned long flags)
{
	return PVRSRVDrmLoad(dev, flags);
}

int
SYSPVROpen(struct drm_device *dev, struct drm_file *pFile)
{
	return PVRSRVDrmOpen(dev, pFile);
}

int
SYSPVRUnload(struct drm_device *dev)
{
	return PVRSRVDrmUnload(dev);
}

void
SYSPVRPostClose(struct drm_device *dev, struct drm_file *file)
{
	return PVRSRVDrmPostClose(dev, file);
}

int
SYSPVRBridgeDispatch(struct drm_device *dev, IMG_VOID *arg, struct drm_file *pFile)
{
	return PVRSRV_BridgeDispatchKM(dev, arg, pFile);
}

int
SYSPVRDCDriverIoctl(struct drm_device *dev, IMG_VOID *arg, struct drm_file *pFile)
{
	return PVRDRM_Dummy_ioctl(dev, arg, pFile);

}

int
SYSPVRBCDriverIoctl(struct drm_device *dev, IMG_VOID *arg, struct drm_file *pFile)
{
	return PVRDRM_Dummy_ioctl(dev, arg, pFile);

}

int
SYSPVRIsMaster(struct drm_device *dev, IMG_VOID *arg, struct drm_file *pFile)
{
	return PVRDRMIsMaster(dev, arg, pFile);
}

int
SYSPVRUnprivCmd(struct drm_device *dev, IMG_VOID *arg, struct drm_file *pFile)
{
	return PVRDRMUnprivCmd(dev, arg, pFile);
}

int
SYSPVRMMap(struct file* pFile, struct vm_area_struct* ps_vma)
{
	int ret;

	ret = PVRMMap(pFile, ps_vma);
	if (ret == -ENOENT)
	{
		ret = drm_mmap(pFile, ps_vma);
	}

	return ret;
}

int
SYSPVRDBGDrivIoctl(struct drm_device *dev, IMG_VOID *arg, struct drm_file *pFile)
{
#if defined(PDUMP)
	return dbgdrv_ioctl(dev, arg, pFile);
#else
	PVR_UNREFERENCED_PARAMETER(dev);
	PVR_UNREFERENCED_PARAMETER(arg);
	PVR_UNREFERENCED_PARAMETER(pFile);

	return -EINVAL;
#endif
}

int SYSPVRSuspend(struct drm_device *dev)
{
	if (PVRSRVSetPowerStateKM(PVRSRV_SYS_POWER_STATE_D3) != PVRSRV_OK)
	{
		return -EBUSY;
	}

#if defined(DISPLAY_CONTROLLER)
        if (PVR_DRM_MAKENAME(DISPLAY_CONTROLLER, _Suspend)(dev) != 0)
        {
		(void)PVRSRVSetPowerStateKM(PVRSRV_SYS_POWER_STATE_D0);
                return -EBUSY;
        }
#else
	PVR_UNREFERENCED_PARAMETER(dev);
#endif

	return 0;
}

int SYSPVRResume(struct drm_device *dev)
{
#if defined(DISPLAY_CONTROLLER)
        if (PVR_DRM_MAKENAME(DISPLAY_CONTROLLER, _Resume)(dev) != 0)
        {
                return -EINVAL;
        }
#else
	PVR_UNREFERENCED_PARAMETER(dev);
#endif

	if (PVRSRVSetPowerStateKM(PVRSRV_SYS_POWER_STATE_D0) != PVRSRV_OK)
	{
#if defined(DISPLAY_CONTROLLER)
		(void)PVR_DRM_MAKENAME(DISPLAY_CONTROLLER, _Suspend)(dev);
#endif
		return -EINVAL;
	}

	return 0;
}
