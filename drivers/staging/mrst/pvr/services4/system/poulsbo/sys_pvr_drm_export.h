/*************************************************************************/ /*!
@Title          DRM definitions
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

#if !defined(__SYS_PVR_DRM_EXPORT_H__)
#define __SYS_PVR_DRM_EXPORT_H__

#include "pvr_drm_shared.h"

#if defined(__KERNEL__)

#include "services_headers.h"
#include "private_data.h"
#include "pvr_drm.h"

#include "pvr_bridge.h"

#if defined(PDUMP)
#include "linuxsrv.h"
#endif

#define PVR_DRM_SRVKM_IOCTL \
	DRM_IOW(DRM_COMMAND_BASE + PVR_DRM_SRVKM_CMD, PVRSRV_BRIDGE_PACKAGE)

#define PVR_DRM_DISP_IOCTL \
	DRM_IO(DRM_COMMAND_BASE + PVR_DRM_DISP_CMD)

#define PVR_DRM_BC_IOCTL \
	DRM_IO(DRM_COMMAND_BASE + PVR_DRM_BC_CMD)

#define	PVR_DRM_IS_MASTER_IOCTL \
	DRM_IO(DRM_COMMAND_BASE + PVR_DRM_IS_MASTER_CMD)

#define	PVR_DRM_UNPRIV_IOCTL \
	DRM_IOWR(DRM_COMMAND_BASE + PVR_DRM_UNPRIV_CMD, IMG_UINT32)

#if defined(PDUMP)
#define	PVR_DRM_DBGDRV_IOCTL \
	DRM_IOW(DRM_COMMAND_BASE + PVR_DRM_DBGDRV_CMD, IOCTL_PACKAGE)
#else
#define	PVR_DRM_DBGDRV_IOCTL \
	DRM_IO(DRM_COMMAND_BASE + PVR_DRM_DBGDRV_CMD)
#endif

int SYSPVRInit(void);
int SYSPVRLoad(struct drm_device *dev, unsigned long flags);
int SYSPVROpen(struct drm_device *dev, struct drm_file *pFile);
int SYSPVRUnload(struct drm_device *dev);
void SYSPVRPostClose(struct drm_device *dev, struct drm_file *file);
int SYSPVRBridgeDispatch(struct drm_device *dev, IMG_VOID *arg, struct drm_file *pFile);
int SYSPVRDCDriverIoctl(struct drm_device *dev, IMG_VOID *arg, struct drm_file *pFile);
int SYSPVRBCDriverIoctl(struct drm_device *dev, IMG_VOID *arg, struct drm_file *pFile);
int SYSPVRIsMaster(struct drm_device *dev, IMG_VOID *arg, struct drm_file *pFile);
int SYSPVRUnprivCmd(struct drm_device *dev, IMG_VOID *arg, struct drm_file *pFile);

int SYSPVRMMap(struct file* pFile, struct vm_area_struct* ps_vma);

int SYSPVRDBGDrivIoctl(struct drm_device *dev, IMG_VOID *arg, struct drm_file *pFile);

int SYSPVRServiceSGXInterrupt(struct drm_device *dev);

int SYSPVRSuspend(struct drm_device *dev);
int SYSPVRResume(struct drm_device *dev);

#endif	/* __KERNEL__ */

#endif	/* __SYS_PVR_DRM_EXPORT_H__ */

