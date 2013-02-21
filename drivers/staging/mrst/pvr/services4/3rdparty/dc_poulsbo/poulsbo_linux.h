/*************************************************************************/ /*!
@Title          Poulsbo Linux Display driver interface.
@Copyright      Copyright (c) Imagination Technologies Ltd. All Rights Reserved
@Description    Poulsbo Linux Display kernel driver structures and prototypes.
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

#if !defined(__POULSBO_LINUX_H__)
#define __POULSBO_LINUX_H__

#include "dc_poulsbo.h"

#if defined(SUPPORT_DRI_DRM) 
#include <linux/kernel.h>
#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#endif

#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>

#if defined(__cplusplus)
extern "C" {
#endif

#if defined(SUPPORT_DRI_DRM)
#define to_pvr_crtc(crtc)		container_of(crtc, PVRPSB_CRTC, sCrtc)
#define to_pvr_connector(connector)	container_of(connector, PVRPSB_CONNECTOR, sConnector)
#define to_pvr_encoder(encoder)		container_of(encoder, PVRPSB_ENCODER, sEncoder)

typedef struct PVRPSB_CRTC_TAG
{
	struct drm_crtc			sCrtc;

	IMG_UINT32			ui32Pipe;
} PVRPSB_CRTC;

typedef struct PVRPSB_CONNECTOR_TAG
{
	struct drm_connector		sConnector;

	struct i2c_adapter		*psAdapter;

	IMG_UINT32			ui32Port;
} PVRPSB_CONNECTOR;

typedef struct PVRPSB_ENCODER_TAG
{
	struct drm_encoder		sEncoder;

	IMG_UINT16			ui16OutputType;
} PVRPSB_ENCODER;


struct drm_connector *PVRGetConnectorForEncoder(struct drm_encoder *psEncoder);


/******************************************************************************
 * SDVO interface
 *****************************************************************************/
void SDVOSaveState(PVRPSB_DEVINFO *psDevInfo, PVRPSB_STATE *psState);
void SDVORestoreState(PVRPSB_DEVINFO *psDevInfo, PVRPSB_STATE *psState);

PSB_ERROR SDVOSetup(PVRPSB_DEVINFO *psDevInfo);
#endif /* defined(SUPPORT_DRI_DRM) */

/******************************************************************************
 * I2C interface
 *****************************************************************************/
/* Information about the port address, which will be used by the I2C bus. */
typedef struct I2C_INFO_TAG
{
	PVRPSB_DEVINFO		*psDevInfo;
	IMG_UINT32		ui32Offset;
	IMG_UINT32		ui32Addr;

	struct i2c_algorithm	sAlgorithms;
} PVRI2C_INFO;

struct i2c_adapter *PVRI2CAdapterCreate(PVRPSB_DEVINFO *psDevInfo, IMG_UINT32 ui32GPIOPort, IMG_UINT32 ui32Addr);
IMG_VOID PVRI2CAdapterDestroy(struct i2c_adapter *psAdapter);

#if defined(__cplusplus)
}
#endif

#endif /* !defined(__POULSBO_LINUX_H__) */

/******************************************************************************
 End of file (poulsbo_linux.h)
******************************************************************************/
