/*************************************************************************/ /*!
@Title          Poulsbo linux-specific functions
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

/******************************************************************************
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
******************************************************************************/

#include <linux/version.h>

#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,38))
#if !defined(AUTOCONF_INCLUDED)
#include <linux/config.h>
#endif
#endif

#include "poulsbo_linux.h"

#include <linux/module.h>

#if defined(SUPPORT_DRI_DRM)
#include "pvr_drm.h"
#include "3rdparty_dc_drm_shared.h"
#include <drm/drm_mode.h>
#else
#include "pvrmodule.h"
#endif /* defined(SUPPORT_DRI_DRM) */

#include <linux/delay.h>

#if !defined(SUPPORT_DRI_DRM)
struct GTF_TIMINGS_DEF
{
	IMG_UINT32 ui32Width;
	IMG_UINT32 ui32Height;
	IMG_UINT32 ui32RefreshRate;
	IMG_UINT32 ui32Doc;

	IMG_UINT32 ui32Hfront;
	IMG_UINT32 ui32Hsync;
	IMG_UINT32 ui32Hblanking;

	IMG_UINT32 ui32Vfront;
	IMG_UINT32 ui32Vsync;
	IMG_UINT32 ui32Vblanking;
};

/* Definition of GTF timings */
static const struct GTF_TIMINGS_DEF gsGtfTimings[] = 
{
	/* 0: 640 x 480 @ 75HZ */
	{ 640, 480, 75, 31500, 16, 64, 200, 1, 3, 20, },
	/* 1: 800 x 600 @ 56HZ */
	{ 800, 600, 56,	36000, 24, 72, 224, 1, 2, 25, },
	/* 2: 800 x 600 @ 72HZ */
	{ 800, 600,	72,	50000, 56, 120,	240, 37, 6, 66, },
	/* 3: 1024 x 768 @ 75HZ */
	{ 1024,	768, 75, 78800,	16, 96,	288, 1,	3, 32, },
	/* 4: 1024 x 768 @ 70HZ */
	{ 1024,	768, 70, 75000,	24, 136, 304, 3, 6, 38,	},
	/* 5: 1024 x 768 @ 60HZ */
	{ 1024, 768, 60, 65000,	24, 136, 320, 3, 6, 38, },
	/* 6: 1024 x 768 @ 100HZ */
	{ 1024,	768, 100, 113310, 72, 112, 368,	1, 3, 46, },
	/* 7: 1152 x 864 @ 75HZ */
	{ 1152,	864, 75, 108000, 64, 128, 448, 1, 3, 36, },
	/* 8: 1280 x 1024 @ 75HZ */
	{ 1280,	1024, 75, 135000, 16, 144, 408,	1, 3, 42, },
	/* 9: 1280 x 1024 @ 60HZ */
	{ 1280,	1024, 60, 108000, 48, 112, 408,	1, 3, 42, },
	/* 10: 1440 x 900 @ 60HZ */
	{ 1440, 900, 60, 106470, 80, 152, 464, 1, 3, 32, },
};
#endif

#define VMALLOC_TO_PAGE_PHYS(addr)	page_to_phys(vmalloc_to_page(addr))

#if !defined(SUPPORT_DRI_DRM)
MODULE_SUPPORTED_DEVICE(DRVNAME);
#endif

/*******************************************************************************
 * I2C interface
 ******************************************************************************/

/*****************************************************************************
 Function Name:	I2CGetSda - Get SDA line status. 1 means low; 0 means high.
 		I2CGetScl - Get SCL line status. 1 means low; 0 means high.
		I2CSetSda - Set SDA line. 1 to set high; 0 to set low.
		I2CSetScl - Set SCL line. 1 to set high; 0 to set low.
 Description  :	Four helper funtions allowing access to the GPIO pins.
		These helper functions will be called by the registered
		"bit banging" i2c algorithm implemented in Linux Kernel.
*****************************************************************************/
static IMG_INT32 I2CGetSda(IMG_VOID *pvData)
{
	PVRI2C_INFO *psI2CInfo = (PVRI2C_INFO *)pvData;
	IMG_UINT32 ui32In;

	ui32In = PVROSReadMMIOReg(psI2CInfo->psDevInfo, psI2CInfo->ui32Offset);

	return PVRPSB_GPIO_DATA_IN_GET(ui32In);
}

static IMG_INT32 I2CGetScl(IMG_VOID *pvData)
{
	PVRI2C_INFO *psI2CInfo = (PVRI2C_INFO *)pvData;
	IMG_UINT32 ui32In;

	ui32In = PVROSReadMMIOReg(psI2CInfo->psDevInfo, psI2CInfo->ui32Offset);

	return PVRPSB_GPIO_CLOCK_IN_GET(ui32In);
}

static IMG_VOID I2CSetSda(IMG_VOID *pvData, IMG_INT32 ui32State)
{
	PVRI2C_INFO *psI2CInfo = (PVRI2C_INFO *)pvData;
	IMG_UINT32 ui32RegValue = 0;

	ui32RegValue = PVRPSB_GPIO_DATA_DIR_MASK_SET(ui32RegValue, PVRPSB_GPIO_DATA_DIR_MASK_WRITE_VALUE);

	if (ui32State != 0)
	{
		ui32RegValue = PVRPSB_GPIO_DATA_DIR_VALUE_SET(ui32RegValue, PVRPSB_GPIO_DATA_DIR_VALUE_IN);
		ui32RegValue = PVRPSB_GPIO_DATA_MASK_SET(ui32RegValue, PVRPSB_GPIO_DATA_MASK_NOWRITE_VALUE);
	}
	else
	{
		ui32RegValue = PVRPSB_GPIO_DATA_DIR_VALUE_SET(ui32RegValue, PVRPSB_GPIO_DATA_DIR_VALUE_OUT);
		ui32RegValue = PVRPSB_GPIO_DATA_MASK_SET(ui32RegValue, PVRPSB_GPIO_DATA_MASK_WRITE_VALUE);
	}

	PVROSWriteMMIOReg(psI2CInfo->psDevInfo, psI2CInfo->ui32Offset, ui32RegValue);
}

static IMG_VOID I2CSetScl(IMG_VOID *pvData, IMG_INT32 ui32State)
{
	PVRI2C_INFO *psI2CInfo = (PVRI2C_INFO *)pvData;
	IMG_UINT32 ui32RegValue = 0;

	ui32RegValue = PVRPSB_GPIO_CLOCK_DIR_MASK_SET(ui32RegValue, PVRPSB_GPIO_CLOCK_DIR_MASK_WRITE_VALUE);

	if (ui32State != 0)
	{
		ui32RegValue = PVRPSB_GPIO_CLOCK_DIR_VALUE_SET(ui32RegValue, PVRPSB_GPIO_CLOCK_DIR_VALUE_IN);
		ui32RegValue = PVRPSB_GPIO_CLOCK_MASK_SET(ui32RegValue, PVRPSB_GPIO_CLOCK_MASK_NOWRITE_VALUE);
	}
	else
	{
		ui32RegValue = PVRPSB_GPIO_CLOCK_DIR_VALUE_SET(ui32RegValue, PVRPSB_GPIO_CLOCK_DIR_VALUE_OUT);
		ui32RegValue = PVRPSB_GPIO_CLOCK_MASK_SET(ui32RegValue, PVRPSB_GPIO_CLOCK_MASK_WRITE_VALUE);
	}

	PVROSWriteMMIOReg(psI2CInfo->psDevInfo, psI2CInfo->ui32Offset, ui32RegValue);
}

static int I2CMasterXfer(struct i2c_adapter *psAdapter, struct i2c_msg aMessages[], int iNumMessages)
{
	struct i2c_algo_bit_data *psAlgoData	= (struct i2c_algo_bit_data *)psAdapter->algo_data;
	PVRI2C_INFO *psI2CInfo			= (PVRI2C_INFO *)psAlgoData->data;

	if (aMessages[0].addr != psI2CInfo->ui32Addr)
	{
		IMG_UINT8 aui8Buf[4] = {PVRPSB_I2C_ARG0, 0x02, PVRPSB_I2C_CMD, PVRPSB_I2C_CMD_BUSSWITCH};
		struct i2c_msg aConfigMsg[2];
		int iReturn;

		aConfigMsg[0].addr	= psI2CInfo->ui32Addr;
		aConfigMsg[0].flags	= 0;
		aConfigMsg[0].len	= 2;
		aConfigMsg[0].buf	= &aui8Buf[0];

		aConfigMsg[1].addr	= psI2CInfo->ui32Addr;
		aConfigMsg[1].flags	= 0;
		aConfigMsg[1].len	= 2;
		aConfigMsg[1].buf	= &aui8Buf[2];

		iReturn = psI2CInfo->sAlgorithms.master_xfer(psAdapter, aConfigMsg, 2);
		if (iReturn != 2)
		{
			return iReturn;
		}
	}

	return psI2CInfo->sAlgorithms.master_xfer(psAdapter, aMessages, iNumMessages);
}

static u32 I2CFunctionality(struct i2c_adapter *psAdapter)
{
	struct i2c_algo_bit_data *psAlgoData	= (struct i2c_algo_bit_data *)psAdapter->algo_data;
	PVRI2C_INFO *psI2CInfo			= (PVRI2C_INFO *)psAlgoData->data;

	return psI2CInfo->sAlgorithms.functionality(psAdapter);
}

static const struct i2c_algorithm sPSBI2CAlgorithm = 
{
	.master_xfer	= I2CMasterXfer,
	.functionality	= I2CFunctionality,
};

struct i2c_adapter *PVRI2CAdapterCreate(PVRPSB_DEVINFO *psDevInfo, IMG_UINT32 ui32GPIOPort, IMG_UINT32 ui32Addr)
{
	struct i2c_adapter *psAdapter;
	struct i2c_algo_bit_data *psAlgoData;
	PVRI2C_INFO *psI2CInfo;
	int iError;

	psAdapter = (struct i2c_adapter *)kzalloc(sizeof(struct i2c_adapter), GFP_KERNEL);
	if (psAdapter == NULL)
	{
		return NULL;
	}

	/* Allocate and setup the i2c information. This will be passed to our SDA and SCL functions */
	psI2CInfo = (PVRI2C_INFO *)PVROSAllocKernelMem(sizeof(PVRI2C_INFO));
	if (psI2CInfo == NULL)
	{
		goto ExitAdapter;
	}

	psI2CInfo->psDevInfo		= psDevInfo;
	psI2CInfo->ui32Offset		= ui32GPIOPort;
	psI2CInfo->ui32Addr		= ui32Addr;

	/* Allocate and setup the I2C algorithm bit data */
	psAlgoData = (struct i2c_algo_bit_data *)PVROSAllocKernelMem(sizeof(struct i2c_algo_bit_data));
	if (psAlgoData == NULL)
	{
		goto ExitInfo;
	}

	/* Data to be passed in the I2C algorithm */
	psAlgoData->data = psI2CInfo;

	/* Holding time for the SDA/SCL line. Min holding time in the spec is 5ms */
	psAlgoData->udelay = 20;

	/* I2C slave device may hold SCL line low when it has to deal with its interrupts, etc. 
	   Master must stop during this time. The 'timeout' specifies the expire time in 
	   jiffies that the I2C master has to wait for the SCL line to go back to high. */
	psAlgoData->timeout = 5;

	psAlgoData->setsda = I2CSetSda;
	psAlgoData->setscl = I2CSetScl;
	psAlgoData->getsda = I2CGetSda;
	psAlgoData->getscl = I2CGetScl;

	/* Finally setup the adapter */
	psAdapter->owner	= THIS_MODULE;
	psAdapter->algo_data	= psAlgoData;

	if (ui32GPIOPort == PVRPSB_GPIO_E)
	{
		struct i2c_adapter sTempAdapter = {0};

		/* For SDVO there are cases were we need to do a bus switch (e.g. obtaining EDID info). 
		   Instead of doing this explicitly we setup our own I2C master transfer function. 
		   Within this function we make use of the generic master transfer function which 
		   we extract by setting up a temporary I2C adapter. */
		sTempAdapter.owner = THIS_MODULE;
		if (i2c_bit_add_bus(&sTempAdapter))
		{
			goto ExitInfo;
		}

		/* Extract functions from temporary adapter. */
		psI2CInfo->sAlgorithms.master_xfer	= sTempAdapter.algo->master_xfer;
		psI2CInfo->sAlgorithms.functionality	= sTempAdapter.algo->functionality;
	
		/* We have no more use for the temporary adapter so delete it */
		i2c_del_adapter(&sTempAdapter);

		psAdapter->algo	= &sPSBI2CAlgorithm;
		strlcpy(psAdapter->name, "PSB I2C Adapter (SDVO)", sizeof(psAdapter->name));

		iError = i2c_add_adapter(psAdapter);
	}
	else
	{
		strlcpy(psAdapter->name, "PSB I2C Adapter (LVDS)", sizeof(psAdapter->name));

		iError = i2c_bit_add_bus(psAdapter);
	}

	if (iError == 0)
	{
		return psAdapter;
	}

	PVROSFreeKernelMem(psAlgoData);

ExitInfo:
	PVROSFreeKernelMem(psI2CInfo);
		
ExitAdapter:
	PVROSFreeKernelMem(psAdapter);

	return NULL;
}

IMG_VOID PVRI2CAdapterDestroy(struct i2c_adapter *psAdapter)
{
	struct i2c_algo_bit_data *psAlgoData	= (struct i2c_algo_bit_data *)psAdapter->algo_data;
	PVRI2C_INFO *psPortData			= (PVRI2C_INFO *)psAlgoData->data;

	i2c_del_adapter(psAdapter);

	PVROSFreeKernelMem(psPortData);
	PVROSFreeKernelMem(psAlgoData);
	PVROSFreeKernelMem(psAdapter);
}

#if defined(SUPPORT_DRI_DRM)
struct drm_connector *PVRGetConnectorForEncoder(struct drm_encoder *psEncoder)
{
	struct drm_connector *psConnector;
	struct drm_connector *psConnectorTemp;
	int i;

	list_for_each_entry_safe(psConnector, psConnectorTemp, &psEncoder->dev->mode_config.connector_list, head)
	{
		for (i = 0; i < DRM_CONNECTOR_MAX_ENCODER; i++)
		{
			if (psConnector->encoder_ids[i] == psEncoder->base.id)
			{
				return psConnector;
			}
		}
	}

	return NULL;
}

static IMG_VOID CurrentModeStride(PVRPSB_DEVINFO *psDevInfo, IMG_UINT32 *pui32Stride)
{
	if (psDevInfo->ui32ActivePipe == PVRPSB_PIPE_A)
	{
		*pui32Stride = PVROSReadMMIOReg(psDevInfo, PVRPSB_DSPASTRIDE);
	}
	else
	{
		*pui32Stride = PVROSReadMMIOReg(psDevInfo, PVRPSB_DSPBSTRIDE);
	}
}

static IMG_VOID CurrentModePixelFormat(PVRPSB_DEVINFO *psDevInfo, PVRSRV_PIXEL_FORMAT *pePixelFormat)
{
	IMG_UINT32 ui32PlaneControl;

	if (psDevInfo->ui32ActivePipe == PVRPSB_PIPE_A)
	{
		ui32PlaneControl = PVROSReadMMIOReg(psDevInfo, PVRPSB_DSPACNTR);
	}
	else
	{
		ui32PlaneControl = PVROSReadMMIOReg(psDevInfo, PVRPSB_DSPBCNTR);
	}

	/* Map the plane's native format to a PVRSRV format */
	switch (PVRPSB_DSPCNTR_FORMAT_GET(ui32PlaneControl))
	{
		case 2:
		{
			/* 8bpp indexed */
			*pePixelFormat = PVRSRV_PIXEL_FORMAT_PAL8;
			break;
		}
		case 5:
		{
			/* 16-bit BGRX (5:6:5:0) pixel format (XGA compatible) */
			*pePixelFormat = PVRSRV_PIXEL_FORMAT_RGB565;
			break;
		}
		case 6:
		{
			/* 32-bit ARGB (8:8:8:8) pixel format. Ignore alpha. */
			*pePixelFormat = PVRSRV_PIXEL_FORMAT_ARGB8888;
			break;
		}
		default:
		{
			*pePixelFormat = PVRSRV_PIXEL_FORMAT_UNKNOWN;
		}
	}
}

static void SaveState(PVRPSB_DEVINFO *psDevInfo, PVRPSB_STATE *psState)
{
	int i;

	/* Save some non-standard PCI config state. Services will save off the standard PCI config state */
	psState->sPciRegisters.ui16GraphicsControl	= PVROSPciReadWord(psDevInfo, PVRPSB_PCIREG_GC);
	psState->sPciRegisters.ui32StolenMemBase	= PVROSPciReadDWord(psDevInfo, PVRPSB_PCIREG_BSM);
	psState->sPciRegisters.ui32ASLStorage		= PVROSPciReadDWord(psDevInfo, PVRPSB_PCIREG_ASLS);

	/* Save some display related registers */
	psState->sDevRegisters.ui32DspArb	= PVROSReadMMIOReg(psDevInfo, PVRPSB_DSPARB);
	psState->sDevRegisters.ui32DspFW1	= PVROSReadMMIOReg(psDevInfo, PVRPSB_DSPFW1);
	psState->sDevRegisters.ui32DspFW2	= PVROSReadMMIOReg(psDevInfo, PVRPSB_DSPFW2);
	psState->sDevRegisters.ui32DspFW3	= PVROSReadMMIOReg(psDevInfo, PVRPSB_DSPFW3);
	psState->sDevRegisters.ui32DspFW4	= PVROSReadMMIOReg(psDevInfo, PVRPSB_DSPFW4);
	psState->sDevRegisters.ui32DspFW5	= PVROSReadMMIOReg(psDevInfo, PVRPSB_DSPFW5);
	psState->sDevRegisters.ui32DspFW6	= PVROSReadMMIOReg(psDevInfo, PVRPSB_DSPFW6);
	psState->sDevRegisters.ui32DspChicken	= PVROSReadMMIOReg(psDevInfo, PVRPSB_DSPCHICKEN);

	/* Save display timing related registers */
	psState->sDevRegisters.ui32FpA0		= PVROSReadMMIOReg(psDevInfo, PVRPSB_FPA0);
	psState->sDevRegisters.ui32FpA1		= PVROSReadMMIOReg(psDevInfo, PVRPSB_FPA1);
	psState->sDevRegisters.ui32DpllACtl	= PVROSReadMMIOReg(psDevInfo, PVRPSB_DPLLA_CTL);

	/* Save pipe related registers */
	psState->sDevRegisters.ui32PipeAConf	= PVROSReadMMIOReg(psDevInfo, PVRPSB_PIPEACONF);
	psState->sDevRegisters.ui32PipeASrc	= PVROSReadMMIOReg(psDevInfo, PVRPSB_PIPEASRC);
	psState->sDevRegisters.ui32HTotalA	= PVROSReadMMIOReg(psDevInfo, PVRPSB_HTOTALA);
	psState->sDevRegisters.ui32HBlankA	= PVROSReadMMIOReg(psDevInfo, PVRPSB_HBLANKA);
	psState->sDevRegisters.ui32HSyncA	= PVROSReadMMIOReg(psDevInfo, PVRPSB_HSYNCA);
	psState->sDevRegisters.ui32VTotalA	= PVROSReadMMIOReg(psDevInfo, PVRPSB_VTOTALA);
	psState->sDevRegisters.ui32VBlankA	= PVROSReadMMIOReg(psDevInfo, PVRPSB_VBLANKA);
	psState->sDevRegisters.ui32VSyncA	= PVROSReadMMIOReg(psDevInfo, PVRPSB_VSYNCA);
	psState->sDevRegisters.ui32BClrPatA	= PVROSReadMMIOReg(psDevInfo, PVRPSB_BCLRPATA);

	for (i = 0; i < PVRPSB_DPALETTE_LEN; i++)
	{
		psState->sDevRegisters.aui32DPaletteA[i] = PVROSReadMMIOReg(psDevInfo, PVRPSB_DPALETTEA + (sizeof(IMG_UINT32) * i));
	}

	/* Save plane related registers */
	psState->sDevRegisters.ui32DspACntr	= PVROSReadMMIOReg(psDevInfo, PVRPSB_DSPACNTR);
	psState->sDevRegisters.ui32DspALinOff	= PVROSReadMMIOReg(psDevInfo, PVRPSB_DSPALINOFF);
	psState->sDevRegisters.ui32DspAStride	= PVROSReadMMIOReg(psDevInfo, PVRPSB_DSPASTRIDE);
	psState->sDevRegisters.ui32DspAPos	= PVROSReadMMIOReg(psDevInfo, PVRPSB_DSPAPOS);
	psState->sDevRegisters.ui32DspASize	= PVROSReadMMIOReg(psDevInfo, PVRPSB_DSPASIZE);
	psState->sDevRegisters.ui32DspASurf	= PVROSReadMMIOReg(psDevInfo, PVRPSB_DSPASURF);
	psState->sDevRegisters.ui32DspATileOff	= PVROSReadMMIOReg(psDevInfo, PVRPSB_DSPATILEOFF);

	/* Save HW cursor related registers */
	psState->sDevRegisters.ui32CurACntr	= PVROSReadMMIOReg(psDevInfo, PVRPSB_CURACNTR);
	psState->sDevRegisters.ui32CurABase	= PVROSReadMMIOReg(psDevInfo, PVRPSB_CURABASE);
	psState->sDevRegisters.ui32CurAPos	= PVROSReadMMIOReg(psDevInfo, PVRPSB_CURAPOS);
	psState->sDevRegisters.ui32CurAPalet0	= PVROSReadMMIOReg(psDevInfo, PVRPSB_CURAPALET0);
	psState->sDevRegisters.ui32CurAPalet1	= PVROSReadMMIOReg(psDevInfo, PVRPSB_CURAPALET1);
	psState->sDevRegisters.ui32CurAPalet2	= PVROSReadMMIOReg(psDevInfo, PVRPSB_CURAPALET2);
	psState->sDevRegisters.ui32CurAPalet3	= PVROSReadMMIOReg(psDevInfo, PVRPSB_CURAPALET3);

	psState->sDevRegisters.ui32VGACtl	= PVROSReadMMIOReg(psDevInfo, PVRPSB_VGA_CTL);

	/* Save HW overlay related registers */
	psState->sDevRegisters.ui32OvAdd	= PVROSReadMMIOReg(psDevInfo, PVRPSB_OVADD);
	psState->sDevRegisters.ui32OGamC5	= PVROSReadMMIOReg(psDevInfo, PVRPSB_OGAMC5);
	psState->sDevRegisters.ui32OGamC4	= PVROSReadMMIOReg(psDevInfo, PVRPSB_OGAMC4);
	psState->sDevRegisters.ui32OGamC3	= PVROSReadMMIOReg(psDevInfo, PVRPSB_OGAMC3);
	psState->sDevRegisters.ui32OGamC2	= PVROSReadMMIOReg(psDevInfo, PVRPSB_OGAMC2);
	psState->sDevRegisters.ui32OGamC1	= PVROSReadMMIOReg(psDevInfo, PVRPSB_OGAMC1);
	psState->sDevRegisters.ui32OGamC0	= PVROSReadMMIOReg(psDevInfo, PVRPSB_OGAMC0);

	/* Save GTT related registers */
	psState->sDevRegisters.ui32PgTblValue	= PVROSReadMMIOReg(psDevInfo, PVRPSB_PGTBL_CTL);

	/* Save the contents of the GTT memory if it's in use */
	if (psState->pui32GTTContents != NULL && psDevInfo->sGTTInfo.pvGTTCPUVAddr != NULL)
	{
		PVROSCopyFromIOMem(psState->pui32GTTContents, psDevInfo->sGTTInfo.pvGTTCPUVAddr, psDevInfo->sGTTInfo.ui32GTTSize);
	}

	SDVOSaveState(psDevInfo, psState);
}

static void RestoreState(PVRPSB_DEVINFO *psDevInfo, PVRPSB_STATE *psState)
{
	int i;

	/* Restore some PCI config state */
	PVROSPciWriteWord(psDevInfo, PVRPSB_PCIREG_GC, psState->sPciRegisters.ui16GraphicsControl);
	PVROSPciWriteDWord(psDevInfo, PVRPSB_PCIREG_BSM, psState->sPciRegisters.ui32StolenMemBase);
	PVROSPciWriteDWord(psDevInfo, PVRPSB_PCIREG_ASLS, psState->sPciRegisters.ui32ASLStorage);

	/* Restore GTT related registers */
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_PGTBL_CTL, psState->sDevRegisters.ui32PgTblValue);

	/* Restore the contents of the GTT memory */
	if (psState->pui32GTTContents != NULL && psDevInfo->sGTTInfo.pvGTTCPUVAddr != NULL)
	{
		PVROSCopyToIOMem(psDevInfo->sGTTInfo.pvGTTCPUVAddr, psState->pui32GTTContents, psDevInfo->sGTTInfo.ui32GTTSize);
	}

	/* Restore some display related registers */
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPARB, psState->sDevRegisters.ui32DspArb);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPFW1, psState->sDevRegisters.ui32DspFW1);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPFW2, psState->sDevRegisters.ui32DspFW2);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPFW3, psState->sDevRegisters.ui32DspFW3);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPFW4, psState->sDevRegisters.ui32DspFW4);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPFW5, psState->sDevRegisters.ui32DspFW5);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPFW6, psState->sDevRegisters.ui32DspFW6);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPCHICKEN, psState->sDevRegisters.ui32DspChicken);

	/* Restore VGA register */
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_VGA_CTL, psState->sDevRegisters.ui32VGACtl);

	/* Restore display timing related registers */
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_FPA0, psState->sDevRegisters.ui32FpA0);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_FPA1, psState->sDevRegisters.ui32FpA1);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_DPLLA_CTL, psState->sDevRegisters.ui32DpllACtl);

	PVROSDelayus(150);

	/* Restore pipe related registers */
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_PIPEACONF, PVRPSB_PIPECONF_ENABLE_SET(psState->sDevRegisters.ui32PipeAConf, 0));
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_PIPEASRC, psState->sDevRegisters.ui32PipeASrc);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_HTOTALA, psState->sDevRegisters.ui32HTotalA);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_HBLANKA, psState->sDevRegisters.ui32HBlankA);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_HSYNCA, psState->sDevRegisters.ui32HSyncA);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_VTOTALA, psState->sDevRegisters.ui32VTotalA);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_VBLANKA, psState->sDevRegisters.ui32VBlankA);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_VSYNCA, psState->sDevRegisters.ui32VSyncA);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_BCLRPATA, psState->sDevRegisters.ui32BClrPatA);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_PIPEACONF, psState->sDevRegisters.ui32PipeAConf);

	for (i = 0; i < PVRPSB_DPALETTE_LEN; i++)
	{
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_DPALETTEA + (sizeof(IMG_UINT32) * i), psState->sDevRegisters.aui32DPaletteA[i]);
	}

	/* Restore plane related registers */
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPACNTR, PVRPSB_DSPCNTR_ENABLE_SET(psState->sDevRegisters.ui32DspACntr, 0));
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPALINOFF, psState->sDevRegisters.ui32DspALinOff);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPASTRIDE, psState->sDevRegisters.ui32DspAStride);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPAPOS, psState->sDevRegisters.ui32DspAPos);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPASIZE, psState->sDevRegisters.ui32DspASize);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPASURF, psState->sDevRegisters.ui32DspASurf);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPATILEOFF, psState->sDevRegisters.ui32DspATileOff);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPACNTR, psState->sDevRegisters.ui32DspACntr);

	/* Restore HW cursor related registers */
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_CURAPALET0, psState->sDevRegisters.ui32CurAPalet0);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_CURAPALET1, psState->sDevRegisters.ui32CurAPalet1);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_CURAPALET2, psState->sDevRegisters.ui32CurAPalet2);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_CURAPALET3, psState->sDevRegisters.ui32CurAPalet3);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_CURAPOS, psState->sDevRegisters.ui32CurAPos);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_CURACNTR, psState->sDevRegisters.ui32CurACntr);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_CURABASE, psState->sDevRegisters.ui32CurABase);

	/* Restore HW overlay related registers */
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_OGAMC5, psState->sDevRegisters.ui32OGamC5);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_OGAMC4, psState->sDevRegisters.ui32OGamC4);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_OGAMC3, psState->sDevRegisters.ui32OGamC3);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_OGAMC2, psState->sDevRegisters.ui32OGamC2);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_OGAMC1, psState->sDevRegisters.ui32OGamC1);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_OGAMC0, psState->sDevRegisters.ui32OGamC0);
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_OVADD, psState->sDevRegisters.ui32OvAdd);

	SDVORestoreState(psDevInfo, psState);
}

/*******************************************************************************
 * DRM mode config functions
 ******************************************************************************/

static struct drm_framebuffer *ModeConfigUserFbCreate(struct drm_device *psDrmDev, struct drm_file *psFile, struct drm_mode_fb_cmd *psModeCommand)
{
	/* Not supported */
	return NULL;
}

static int ModeConfigFbChanged(struct drm_device *psDrmDev)
{
	/* Not supported */
	return -ENOSYS;
}

static const struct drm_mode_config_funcs sModeConfigFuncs = 
{
	.fb_create	= ModeConfigUserFbCreate, 
	.fb_changed	= ModeConfigFbChanged, 
};

/******************************************************************************
 * CRTC functions
 ******************************************************************************/

/* Control power levels on the CRTC. If the mode passed in is
   unsupported, the provider must use the next lowest power level. */
static void CrtcHelperDpms(struct drm_crtc *psCrtc, int iMode)
{
	PVRPSB_CRTC *psPvrCrtc = to_pvr_crtc(psCrtc);

	if (psPvrCrtc->ui32Pipe == PVRPSB_PIPE_A)
	{
		PVRPSB_DEVINFO *psDevInfo = (PVRPSB_DEVINFO *)psCrtc->dev->dev_private;
		IMG_UINT32 ui32RegVal;
		switch (iMode)
		{
			case DRM_MODE_DPMS_ON:
			{
				/* Enable the Pipe */
				ui32RegVal = PVROSReadMMIOReg(psDevInfo, PVRPSB_PIPEACONF);
				ui32RegVal = PVRPSB_PIPECONF_ENABLE_SET(ui32RegVal, 1);
				PVROSWriteMMIOReg(psDevInfo, PVRPSB_PIPEACONF, ui32RegVal);

				PVROSDelayus(20000);

				/* Enable the planes */
				ui32RegVal = PVRPSB_PIPECONF_PLANES_OFF_SET(ui32RegVal, 0);
				ui32RegVal = PVRPSB_PIPECONF_CURSOR_OFF_SET(ui32RegVal, 0);
				PVROSWriteMMIOReg(psDevInfo, PVRPSB_PIPEACONF, ui32RegVal);

				ui32RegVal = PVROSReadMMIOReg(psDevInfo, PVRPSB_DSPACNTR);
				ui32RegVal = PVRPSB_DSPCNTR_ENABLE_SET(ui32RegVal, 1);
				PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPACNTR, ui32RegVal);

				/* Enable the port just in case it was disabled for some reason... */
				ui32RegVal = PVROSReadMMIOReg(psDevInfo, PVRPSB_SDVOB_CTL);
				ui32RegVal = PVRPSB_SDVO_CTL_ENABLE_SET(ui32RegVal, 1);
				PVROSWriteMMIOReg(psDevInfo, PVRPSB_SDVOB_CTL, ui32RegVal);

				break;
			}
			case DRM_MODE_DPMS_STANDBY:
			case DRM_MODE_DPMS_SUSPEND:
			case DRM_MODE_DPMS_OFF:
			{
				/* Don't disable the port because then we have no way of getting EDID data from the monitor */

				/* Disable planes (VGA or hi-res) */
				ui32RegVal = PVROSReadMMIOReg(psDevInfo, PVRPSB_PIPEACONF);
				ui32RegVal = PVRPSB_PIPECONF_PLANES_OFF_SET(ui32RegVal, 1);
				ui32RegVal = PVRPSB_PIPECONF_CURSOR_OFF_SET(ui32RegVal, 1);
				PVROSWriteMMIOReg(psDevInfo, PVRPSB_PIPEACONF, ui32RegVal);

				/* Disable pipe */
				ui32RegVal = PVROSReadMMIOReg(psDevInfo, PVRPSB_PIPEACONF);
				ui32RegVal = PVRPSB_PIPECONF_ENABLE_SET(ui32RegVal, 0);
				PVROSWriteMMIOReg(psDevInfo, PVRPSB_PIPEACONF, ui32RegVal);

				/* Disable VGA display */
				ui32RegVal = PVROSReadMMIOReg(psDevInfo, PVRPSB_VGA_CTL);
				ui32RegVal = PVRPSB_VGA_CTL_DISABLE_SET(ui32RegVal, 1);
				PVROSWriteMMIOReg(psDevInfo, PVRPSB_VGA_CTL, ui32RegVal);

				/* Wait for planes and pipe to be disabled */
				PVROSDelayus(20000);
				break;
			}
		}
	}
}

static void CrtcHelperPrepare(struct drm_crtc *psCrtc)
{
	PVRPSB_DEVINFO *psDevInfo = (PVRPSB_DEVINFO *)psCrtc->dev->dev_private;

	if (psDevInfo->ui32ActivePipe == PVRPSB_PIPE_A)
	{
		IMG_UINT32 ui32RegVal;

		/* Disable SDVO port stall */
		ui32RegVal = 0;
		ui32RegVal = PVRPSB_SDVO_CTL_ENABLE_SET(ui32RegVal, 1);
		ui32RegVal = PVRPSB_SDVO_CTL_PIPE_SET(ui32RegVal, PVRPSB_PIPE_A);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_SDVOB_CTL, ui32RegVal);

		/* Disable port */
		ui32RegVal = PVROSReadMMIOReg(psDevInfo, PVRPSB_SDVOB_CTL);
		ui32RegVal = PVRPSB_SDVO_CTL_ENABLE_SET(ui32RegVal, 0);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_SDVOB_CTL, ui32RegVal);

		/* Disable planes (VGA or hires) */
		ui32RegVal = PVROSReadMMIOReg(psDevInfo, PVRPSB_PIPEACONF);
		ui32RegVal = PVRPSB_PIPECONF_PLANES_OFF_SET(ui32RegVal, 1);
		ui32RegVal = PVRPSB_PIPECONF_CURSOR_OFF_SET(ui32RegVal, 1);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_PIPEACONF, ui32RegVal);

		/* Disable pipe */
		ui32RegVal = PVROSReadMMIOReg(psDevInfo, PVRPSB_PIPEACONF);
		ui32RegVal = PVRPSB_PIPECONF_ENABLE_SET(ui32RegVal, 0);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_PIPEACONF, ui32RegVal);

		/* Disable VGA display */
		ui32RegVal = PVROSReadMMIOReg(psDevInfo, PVRPSB_VGA_CTL);
		ui32RegVal = PVRPSB_VGA_CTL_DISABLE_SET(ui32RegVal, 1);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_VGA_CTL, ui32RegVal);

		/* Wait for planes and pipe to be disabled */
		PVROSDelayus(20000);
	}
}

static void CrtcHelperCommit(struct drm_crtc *psCrtc)
{
	PVRPSB_DEVINFO *psDevInfo = (PVRPSB_DEVINFO *)psCrtc->dev->dev_private;

	if (psDevInfo->ui32ActivePipe == PVRPSB_PIPE_A)
	{
		IMG_UINT32 ui32RegVal;

		/* Enable pipe */
		ui32RegVal = PVROSReadMMIOReg(psDevInfo, PVRPSB_PIPEACONF);
		ui32RegVal = PVRPSB_PIPECONF_ENABLE_SET(ui32RegVal, 1);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_PIPEACONF, ui32RegVal);

		/* Wait for the pipe to be enabled */
		PVROSDelayus(20000);

		ui32RegVal = PVROSReadMMIOReg(psDevInfo, PVRPSB_PIPEACONF);
		ui32RegVal = PVRPSB_PIPECONF_PLANES_OFF_SET(ui32RegVal, 0);
		ui32RegVal = PVRPSB_PIPECONF_CURSOR_OFF_SET(ui32RegVal, 0);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_PIPEACONF, ui32RegVal);

		/* Enable the display plane. Use this opportunity to also enforce some defaults 
		   since we don't know what the BIOS might have done. */
		ui32RegVal = PVROSReadMMIOReg(psDevInfo, PVRPSB_DSPACNTR);
		ui32RegVal = PVRPSB_DSPCNTR_ENABLE_SET(ui32RegVal, 1);
		ui32RegVal = PVRPSB_DSPCNTR_GAMMA_SET(ui32RegVal, 0);
		ui32RegVal = PVRPSB_DSPCNTR_FORMAT_SET(ui32RegVal, PVRPSB_DSPCNTR_FORMAT_B8G8R8X8);
		ui32RegVal = PVRPSB_DSPCNTR_PIXELMULTI_SET(ui32RegVal, PVRPSB_DSPCNTR_PIXELMULTI_NONE);
		ui32RegVal = PVRPSB_DSPCNTR_180_SET(ui32RegVal, 0);
		ui32RegVal = PVRPSB_DSPCNTR_TILED_SET(ui32RegVal, 0);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPACNTR, ui32RegVal);

		/* Set the cursor mode and base address. This will re-enable it if it was enabled before the modeset */
		ui32RegVal = PVROSReadMMIOReg(psDevInfo, PVRPSB_CURACNTR);
		ui32RegVal = PVRPSB_CURACNTR_MODE_SET(ui32RegVal, psDevInfo->sCursorInfo.ui32Mode);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_CURACNTR, ui32RegVal);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_CURABASE, PVROSReadMMIOReg(psDevInfo, PVRPSB_CURABASE));

		/* Enable ports */
		ui32RegVal = PVROSReadMMIOReg(psDevInfo, PVRPSB_SDVOB_CTL);
		ui32RegVal = PVRPSB_SDVO_CTL_ENABLE_SET(ui32RegVal, 1);
		ui32RegVal = PVRPSB_SDVO_CTL_PIPE_SET(ui32RegVal, PVRPSB_PIPE_A);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_SDVOB_CTL, ui32RegVal);
	}
}

static bool CrtcHelperModeFixup(struct drm_crtc *psCrtc, struct drm_display_mode *psMode, struct drm_display_mode *psAdjustedMode)
{
	/* Nothing to do here */
	PVR_UNREFERENCED_PARAMETER(psCrtc);
	PVR_UNREFERENCED_PARAMETER(psMode);
	PVR_UNREFERENCED_PARAMETER(psAdjustedMode);

	return true;
}

static int CrtcHelperModeSet(struct drm_crtc *psCrtc, struct drm_display_mode *psMode, struct drm_display_mode *psAdjustedMode, int iX, int iY, struct drm_framebuffer *psOldFB)
{
	PVRPSB_DEVINFO *psDevInfo = (PVRPSB_DEVINFO *)psCrtc->dev->dev_private;

	PVR_UNREFERENCED_PARAMETER(iX);
	PVR_UNREFERENCED_PARAMETER(iY);
	PVR_UNREFERENCED_PARAMETER(psOldFB);

	if (psDevInfo->ui32ActivePipe == PVRPSB_PIPE_A)
	{
		PLL_FREQ sPllFreqInfo;
		IMG_UINT32 ui32DotClockMultiplier;
		IMG_UINT32 ui32PllCtrl;
		IMG_UINT32 ui32Fp0;
		IMG_UINT32 ui32RegVal;
		IMG_UINT32 ui32Stride;

		if (PVRPSBSelectPLLFreq(psMode->clock, &sPllFreqInfo, PSB_FALSE) == PSB_FALSE)
		{
			return -ECANCELED;
		}

		ui32DotClockMultiplier = PVRPSBGetDotClockMultiplier(psMode->clock, PSB_FALSE);

		ui32Fp0 = 0;
		ui32Fp0 = PVRPSB_FP_NDIVISOR_SET(ui32Fp0, sPllFreqInfo.ui32N);
		ui32Fp0 = PVRPSB_FP_M1DIVISOR_SET(ui32Fp0, sPllFreqInfo.ui32M1);
		ui32Fp0 = PVRPSB_FP_M2DIVISOR_SET(ui32Fp0, sPllFreqInfo.ui32M2);

		ui32PllCtrl = 0;
		ui32PllCtrl = PVRPSB_DPLL_ENABLE_SET(ui32PllCtrl, 0);
		ui32PllCtrl = PVRPSB_DPLL_HIGHSPEED_ENABLE_SET(ui32PllCtrl, 1);
		ui32PllCtrl = PVRPSB_DPLL_VGA_DISABLE_SET(ui32PllCtrl, 1);
		ui32PllCtrl = PVRPSB_DPLL_MODE_SET(ui32PllCtrl, PVRPSB_DPLL_MODE_SDVO);
		ui32PllCtrl = PVRPSB_DPLL_P2_DIVIDE_SET(ui32PllCtrl, PVRPSB_DPLL_P2_DIVIDE_SDVO_10);
		ui32PllCtrl = PVRPSB_DPLL_P1_POSTDIVIDE_SET(ui32PllCtrl, (1 << (sPllFreqInfo.ui32P1 - 1)));
		ui32PllCtrl = PVRPSB_DPLL_REF_CLOCK_SET(ui32PllCtrl, PVRPSB_DPLL_REF_CLOCK_DEFAULT);
		ui32PllCtrl = PVRPSB_DPLL_CLOCK_MULTI_SET(ui32PllCtrl, ui32DotClockMultiplier);
		ui32PllCtrl = PVRPSB_DPLL_P2_SET(ui32PllCtrl, ((10 - sPllFreqInfo.ui32P2) / 5));

		/* We have to do this odd sequence to get the values to stick... */
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_FPA0, ui32Fp0);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_DPLLA_CTL, ui32PllCtrl);
		PVROSReadMMIOReg(psDevInfo, PVRPSB_DPLLA_CTL);

		PVROSDelayus(150);

		PVROSWriteMMIOReg(psDevInfo, PVRPSB_FPA0, ui32Fp0);

		ui32PllCtrl = PVRPSB_DPLL_ENABLE_SET(ui32PllCtrl, 1);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_DPLLA_CTL, ui32PllCtrl);
		ui32PllCtrl = PVROSReadMMIOReg(psDevInfo, PVRPSB_DPLLA_CTL);
	
		PVROSDelayus(150);

		PVROSWriteMMIOReg(psDevInfo, PVRPSB_DPLLA_CTL, ui32PllCtrl);
		PVROSReadMMIOReg(psDevInfo, PVRPSB_DPLLA_CTL);
		PVROSDelayus(150);

		ui32RegVal = 0;
		ui32RegVal = PVRPSB_HTOTAL_TOTAL_SET(ui32RegVal, psMode->htotal);
		ui32RegVal = PVRPSB_HTOTAL_ACTIVE_SET(ui32RegVal, drm_mode_width(psMode));
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_HTOTALA, ui32RegVal);

		ui32RegVal = 0;
		ui32RegVal = PVRPSB_HBLANK_END_SET(ui32RegVal, psMode->htotal);
		ui32RegVal = PVRPSB_HBLANK_START_SET(ui32RegVal, drm_mode_width(psMode));
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_HBLANKA, ui32RegVal);

		ui32RegVal = 0;
		ui32RegVal = PVRPSB_HSYNC_END_SET(ui32RegVal, psMode->hsync_end);
		ui32RegVal = PVRPSB_HSYNC_START_SET(ui32RegVal, psMode->hsync_start);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_HSYNCA, ui32RegVal);


		ui32RegVal = 0;
		ui32RegVal = PVRPSB_VTOTAL_TOTAL_SET(ui32RegVal, psMode->vtotal);
		ui32RegVal = PVRPSB_VTOTAL_ACTIVE_SET(ui32RegVal, drm_mode_height(psMode));
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_VTOTALA, ui32RegVal);

		ui32RegVal = 0;
		ui32RegVal = PVRPSB_VBLANK_END_SET(ui32RegVal, psMode->vtotal);
		ui32RegVal = PVRPSB_VBLANK_START_SET(ui32RegVal, drm_mode_height(psMode));
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_VBLANKA, ui32RegVal);

		ui32RegVal = 0;
		ui32RegVal = PVRPSB_VSYNC_END_SET(ui32RegVal, psMode->vsync_end);
		ui32RegVal = PVRPSB_VSYNC_START_SET(ui32RegVal, psMode->vsync_start);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_VSYNCA, ui32RegVal);


		ui32RegVal = 0;
		ui32RegVal = PVRPSB_DSPSIZE_WIDTH_SET(ui32RegVal, drm_mode_width(psMode));
		ui32RegVal = PVRPSB_DSPSIZE_HEIGHT_SET(ui32RegVal, drm_mode_height(psMode));
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPASIZE, ui32RegVal);

		/* Really we should be taking into acount iX and iY and setting this register up accordingly. */
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPAPOS, 0);

		ui32RegVal = 0;
		ui32RegVal = PVRPSB_PIPESRC_HSIZE_SET(ui32RegVal, drm_mode_width(psMode));
		ui32RegVal = PVRPSB_PIPESRC_VSIZE_SET(ui32RegVal, drm_mode_height(psMode));
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_PIPEASRC, ui32RegVal);

		ui32Stride = drm_mode_width(psMode) * 4; /* We should be getting the bpp in a better way instead of assuming it's 4 */

		ui32RegVal = PVRPSB_DSPSTRIDE_STRIDE_SET(ui32RegVal, PVRPSB_ALIGN(ui32Stride, PVRPSB_DSPSTRIDE_LINEAR_MEM));
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPASTRIDE, ui32RegVal);

		PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPALINOFF, 0);

		return 0;
	}
	else
	{
		return -ENOSYS;
	}	
}

static void CrtcHelperLoadLut(struct drm_crtc *psCrtc)
{
	PVR_UNREFERENCED_PARAMETER(psCrtc);
}

static const struct drm_crtc_helper_funcs sCrtcHelperFuncs = 
{
	.dpms		= CrtcHelperDpms, 
	.prepare	= CrtcHelperPrepare, 
	.commit		= CrtcHelperCommit, 
	.mode_fixup	= CrtcHelperModeFixup, 
	.mode_set	= CrtcHelperModeSet, 
	.mode_set_base	= NULL, 
	.load_lut	= CrtcHelperLoadLut, 
};

static int CrtcCursorSet(struct drm_crtc *psCrtc, struct drm_file *psFile, uint32_t ui32Handle, uint32_t ui32Width, uint32_t ui32Height)
{
	PVRPSB_DEVINFO *psDevInfo = (PVRPSB_DEVINFO *)psCrtc->dev->dev_private;
	PVRPSB_CRTC *psPvrCrtc = to_pvr_crtc(psCrtc);
	IMG_UINT32 ui32CursorMode;
	IMG_UINT32 ui32CursorAddr;

	if (ui32Handle == 0)
	{
		/* Disable the cursor */
		ui32CursorMode = PVROSReadMMIOReg(psDevInfo, PVRPSB_CURACNTR);
		ui32CursorMode = PVRPSB_CURACNTR_MODE_SET(ui32CursorMode, PVRPSB_CURACNTR_MODE_OFF);

		ui32CursorAddr = PVROSReadMMIOReg(psDevInfo, PVRPSB_CURABASE);

		psDevInfo->sCursorInfo.ui32Mode = PVRPSB_CURACNTR_MODE_OFF;
	}
	else
	{
		if (ui32Width > PVRPSB_HW_CURSOR_WIDTH || ui32Height > PVRPSB_HW_CURSOR_HEIGHT)
		{
			printk(KERN_ERR DRVNAME " - %s: specified cursor dimensions are too large\n", __FUNCTION__);
			return -EINVAL;
		}

		if (psDevInfo->sCursorInfo.psBuffer == NULL)
		{
			/* We were unable to preallocate memory for the cursor image */
			printk(KERN_ERR DRVNAME " - %s: no space to store cursor\n", __FUNCTION__);

			return -ENOMEM;
		}

		/* Copy the cursor data into our cursor buffer */
		if (copy_from_user(psDevInfo->sCursorInfo.psBuffer->pvCPUVAddr, (void *)ui32Handle, ui32Width * ui32Height * 4) != 0)
		{
			printk(KERN_ERR DRVNAME " - %s: unable to copy cursor data\n", __FUNCTION__);
			return -EINVAL;
		}

		if (psPvrCrtc->ui32Pipe != PVRPSB_PIPE_A)
		{
			printk(KERN_ERR DRVNAME " - %s: tried to enable the cursor on an unsupported pipe\n", __FUNCTION__);
			return -EINVAL;
		}

		psDevInfo->sCursorInfo.ui32Width	= ui32Width;
		psDevInfo->sCursorInfo.ui32Height	= ui32Height;

		/* Enable the cursor */
		ui32CursorMode = PVROSReadMMIOReg(psDevInfo, PVRPSB_CURACNTR);
		ui32CursorMode = PVRPSB_CURACNTR_PIPE_SET(ui32CursorMode, PVRPSB_CURACNTR_PIPEA);

		if (psDevInfo->sCursorInfo.psBuffer->bIsContiguous == IMG_TRUE)
		{
			ui32CursorMode = PVRPSB_CURACNTR_POPUP_SET(ui32CursorMode, 1);
			ui32CursorAddr = psDevInfo->sCursorInfo.psBuffer->uSysAddr.sCont.uiAddr;
		}
		else
		{
			/* We have non-contiguous memory so we have to set the cursor base 
			   address to the device virtual (graphics) address. Additionally we 
			   must disable popup mode (i.e. use hi-res mode) so the base address 
			   is treated as a graphics address. Unfortunately this results 
			   in a corrupt cursor image being displayed. Maybe we are doing something 
			   wrong? */
			return -ENOMEM;

			ui32CursorMode = PVRPSB_CURACNTR_POPUP_SET(ui32CursorMode, 0);
			ui32CursorAddr = psDevInfo->sCursorInfo.psBuffer->sDevVAddr.uiAddr;
		}

		ui32CursorMode = PVRPSB_CURACNTR_GAMMA_SET(ui32CursorMode, 0);
		ui32CursorMode = PVRPSB_CURACNTR_180_SET(ui32CursorMode, 0);
		ui32CursorMode = PVRPSB_CURACNTR_MODE_SET(ui32CursorMode, PVRPSB_CURACNTR_MODE_64_32_ARGB);

		psDevInfo->sCursorInfo.ui32Mode = PVRPSB_CURACNTR_MODE_64_32_ARGB;
	}

	PVROSWriteMMIOReg(psDevInfo, PVRPSB_CURACNTR, ui32CursorMode);

	/* This triggers the cursor registers to be updated (they are double buffered) so needs to be done last. */
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_CURABASE, ui32CursorAddr);

	return 0;
}

static int CrtcCursorMove(struct drm_crtc *psCrtc, int iX, int iY)
{
	PVRPSB_DEVINFO *psDevInfo = (PVRPSB_DEVINFO *)psCrtc->dev->dev_private;
	IMG_UINT32 ui32Position = 0;

	psDevInfo->sCursorInfo.i32X = iX;
	psDevInfo->sCursorInfo.i32Y = iY;

	if (iX < 0)
	{
		ui32Position = PVRPSB_CURAPOS_XSIGN_SET(ui32Position, 1);
	}

	if (iY < 0)
	{
		ui32Position = PVRPSB_CURAPOS_YSIGN_SET(ui32Position, 1);
	}
			
	ui32Position = PVRPSB_CURAPOS_XMAG_SET(ui32Position, ABS(iX));
	ui32Position = PVRPSB_CURAPOS_YMAG_SET(ui32Position, ABS(iY));
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_CURAPOS, ui32Position);

	/* This triggers the cursor registers to be updated (they are double buffered). */
	PVROSWriteMMIOReg(psDevInfo, PVRPSB_CURABASE, PVROSReadMMIOReg(psDevInfo, PVRPSB_CURABASE));

	return 0;
}

static void CrtcGammaSet(struct drm_crtc *psCrtc, u16 *pu16R, u16 *pu16G, u16 *pu16B, uint32_t ui32Size)
{
	PVR_UNREFERENCED_PARAMETER(psCrtc);
	PVR_UNREFERENCED_PARAMETER(pu16R);
	PVR_UNREFERENCED_PARAMETER(pu16G);
	PVR_UNREFERENCED_PARAMETER(pu16B);
	PVR_UNREFERENCED_PARAMETER(ui32Size);
}

static void CrtcDestroy(struct drm_crtc *psCrtc)
{
	PVRPSB_CRTC *psPVRCrtc = to_pvr_crtc(psCrtc);

	drm_crtc_cleanup(psCrtc);
	PVROSFreeKernelMem(psPVRCrtc);
}

static int CrtcSetConfig(struct drm_mode_set *psModeSet)
{
	PVR_UNREFERENCED_PARAMETER(psModeSet);

	return -ENOSYS;
}

static const struct drm_crtc_funcs sCrtcFuncs = 
{
	.save		= NULL,
	.restore	= NULL,
	.cursor_set	= CrtcCursorSet, 
	.cursor_move	= CrtcCursorMove, 
	.gamma_set	= CrtcGammaSet, 
	.destroy	= CrtcDestroy, 
	.set_config	= CrtcSetConfig, 
};

static PVRPSB_CRTC *CrtcCreate(PVRPSB_DEVINFO *psDevInfo, IMG_UINT32 ui32Pipe)
{
	PVRPSB_CRTC *psPVRCrtc;

	psPVRCrtc = (PVRPSB_CRTC *)kzalloc(sizeof(PVRPSB_CRTC), GFP_KERNEL);
	if (psPVRCrtc)
	{
		drm_crtc_init(psDevInfo->psDrmDev, &psPVRCrtc->sCrtc, &sCrtcFuncs);
		drm_crtc_helper_add(&psPVRCrtc->sCrtc, &sCrtcHelperFuncs);

		psPVRCrtc->ui32Pipe = ui32Pipe;
	}

	return psPVRCrtc;
}


/*******************************************************************************
 * OS Specific functions
 ******************************************************************************/

PSB_ERROR PVROSModeSetInit(PVRPSB_DEVINFO *psDevInfo)
{
	struct drm_device *psDrmDev = psDevInfo->psDrmDev;
	struct drm_display_mode *psMode = NULL;
	PVRPSB_CRTC *psPVRCrtc;
	PSB_ERROR eReturn;

#if defined(PVRPSB_WIDTH) && defined(PVRPSB_HEIGHT)
	struct drm_connector *psConnector;
	struct drm_connector *psTempConnector;
	struct drm_display_mode *psCurrentMode;
	struct drm_display_mode *psTempMode;
#endif

	drm_mode_config_init(psDrmDev);

	psDrmDev->mode_config.funcs		= (void *)&sModeConfigFuncs;
	psDrmDev->mode_config.min_width		= PVRPSB_FB_WIDTH_MIN;
	psDrmDev->mode_config.max_width		= PVRPSB_FB_WIDTH_MAX;
	psDrmDev->mode_config.min_height	= PVRPSB_FB_HEIGHT_MIN;
	psDrmDev->mode_config.max_height	= PVRPSB_FB_HEIGHT_MAX;

	/* In theory we can drive two outputs at the same time (there are two pipes). 
	   However, we support only one pipe so create a single CRTC. */
	psPVRCrtc = CrtcCreate(psDevInfo, PVRPSB_PIPE_A);
	if (psPVRCrtc == NULL)
	{
		printk(KERN_ERR DRVNAME " - %s: failed to create a CRTC.\n", __FUNCTION__);

		eReturn = PSB_ERROR_OUT_OF_MEMORY;
		goto ExitConfigCleanup;
	}
	psDevInfo->ui32ActivePipe = psPVRCrtc->ui32Pipe;

	/* Initialise the output */
	eReturn = SDVOSetup(psDevInfo);
	if (eReturn != PSB_OK)
	{
		goto ExitConfigCleanup;
	}

	mutex_lock(&psDrmDev->mode_config.mutex);

	SaveState(psDevInfo, &psDevInfo->sInitialState);

	drm_helper_initial_config(psDrmDev);

	/* Pick an initial mode */
#if defined(PVRPSB_WIDTH) && defined(PVRPSB_HEIGHT)

	/* At present we can't set the mode from user space. Instead we specify the 
	   initial mode at build time. Unfortunately, this creates a mismatch between 
	   the actual mode (set in kernel space) and the the mode the X server thinks we 
	   have. Work around this here. */

	list_for_each_entry_safe(psConnector, psTempConnector, &psDrmDev->mode_config.connector_list, head)
	{
		/* Find the preferred mode and remove DRM_MODE_TYPE_PREFERRED from its type field */
		list_for_each_entry_safe(psCurrentMode, psTempMode, &psConnector->modes, head)
		{
			if (psCurrentMode->type & DRM_MODE_TYPE_PREFERRED)
			{
				psCurrentMode->type &= ~DRM_MODE_TYPE_PREFERRED;
				break;
			}
		}

		/* Try and find a mode that matches the one specified at build time */
		list_for_each_entry_safe(psCurrentMode, psTempMode, &psConnector->modes, head)
		{
			if (drm_mode_width(psCurrentMode) == PVRPSB_WIDTH && 
			    drm_mode_height(psCurrentMode) == PVRPSB_HEIGHT && 
			    drm_mode_vrefresh(psCurrentMode) == PVRPSB_VREFRESH)
			{
				/* We've found a mode so make this the preferred one */
				psCurrentMode->type |= DRM_MODE_TYPE_PREFERRED;

				psMode = psCurrentMode;
				break;
			}
		}
	}

	if (psMode == NULL)
	{
		printk(KERN_ERR DRVNAME " - %s: couldn't find a mode that matches the requested mode of %dx%d@%dHz. Using the desired mode.", 
		       __FUNCTION__, PVRPSB_WIDTH, PVRPSB_HEIGHT, PVRPSB_VREFRESH);

		psPVRCrtc->sCrtc.desired_mode->type |= DRM_MODE_TYPE_PREFERRED;
		psMode = psPVRCrtc->sCrtc.desired_mode;
	}
#else
	psMode = psPVRCrtc->sCrtc.desired_mode;
#endif

	if (psMode == NULL)
	{
		printk(KERN_ERR DRVNAME " - %s: failed to find the desired mode", __FUNCTION__);

		eReturn = PSB_ERROR_GENERIC;
		goto ExitModeConfigMutexUnlock;
	}

	if (psMode != NULL && drm_crtc_helper_set_mode(&psPVRCrtc->sCrtc, psMode, 0, 0, NULL) == false)
	{
		printk(KERN_ERR DRVNAME " - %s: failed to set an initial mode", __FUNCTION__);

		eReturn = PSB_ERROR_GENERIC;
		goto ExitModeConfigMutexUnlock;
	}

	mutex_unlock(&psDevInfo->psDrmDev->mode_config.mutex);

	psDevInfo->sDisplayDims.ui32Width = drm_mode_width(psMode);
	psDevInfo->sDisplayDims.ui32Height = drm_mode_height(psMode);

	CurrentModePixelFormat(psDevInfo, &(psDevInfo->sDisplayFormat.pixelformat));
	CurrentModeStride(psDevInfo, &(psDevInfo->sDisplayDims.ui32ByteStride));

	return PSB_OK;

ExitModeConfigMutexUnlock:
	RestoreState(psDevInfo, &psDevInfo->sInitialState);
	mutex_unlock(&psDevInfo->psDrmDev->mode_config.mutex);

ExitConfigCleanup:
	drm_mode_config_cleanup(psDevInfo->psDrmDev);

	return eReturn;
}

#else /* defined(SUPPORT_DRI_DRM) */

static PSB_BOOL ReadLvdsEdid(PVRPSB_DEVINFO *psDevInfo, IMG_UINT8 *pui8Buffer, IMG_UINT32 ui32BufferLength)
{
	struct i2c_adapter *psAdapter;
	struct i2c_msg asMessage[2];
	IMG_UINT8 ui8OutBuffer = 0;
	IMG_UINT8 aui8Header[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
	PSB_BOOL bResult = PSB_FALSE;

	/* Create an I2C adapter */
	psAdapter = PVRI2CAdapterCreate(psDevInfo, PVRPSB_GPIO_C, 0);
	if (psAdapter == NULL)
	{
		return PSB_FALSE;
	}

	/* Setup the I2C messages to get the EDID data block */
	asMessage[0].addr	= 0x50;
	asMessage[0].flags	= 0;
	asMessage[0].len	= 1;
	asMessage[0].buf	= &ui8OutBuffer;

	asMessage[1].addr	= 0x50;
	asMessage[1].flags	= I2C_M_RD;
	asMessage[1].len	= ui32BufferLength;
	asMessage[1].buf	= pui8Buffer;

	/* Send the messages */
	if (i2c_transfer(psAdapter, asMessage, 2) == 2)
	{
		/* Verify if we have got a valid EDID block. */
		if (!memcmp(pui8Buffer, aui8Header, sizeof(aui8Header)))
		{
			IMG_UINT8 ui8Checksum = 0;

			while (ui32BufferLength--)
			{
				ui8Checksum += pui8Buffer[ui32BufferLength];
			}

			if (ui8Checksum == 0)
			{
				bResult = PSB_TRUE;	
			}
		}
	}

	/* Destroy the adapter now that we're finished */
	PVRI2CAdapterDestroy(psAdapter);

	return bResult;
}

PSB_ERROR PVROSModeSetInit(PVRPSB_DEVINFO *psDevInfo)
{
	IMG_UINT32 ui32Width;
	IMG_UINT32 ui32Height;
	IMG_UINT32 ui32RegVal;

	if (PVRPSB_LVDS_CTL_ENABLE_GET(PVROSReadMMIOReg(psDevInfo, PVRPSB_LVDS_CTL)) != 0)
	{
		IMG_UINT8 *pui8EdidData;

		pui8EdidData = PVROSAllocKernelMem(128);
		if (pui8EdidData == NULL)
		{
			printk(KERN_ERR DRVNAME " - %s: Failed to allocate EDID data block\n", __FUNCTION__);
			
			return PSB_ERROR_OUT_OF_MEMORY;
		}

		if (ReadLvdsEdid(psDevInfo, pui8EdidData, 128) == PSB_FALSE)
		{
			printk(KERN_ERR DRVNAME " - %s: Failed to get EDID data block\n", __FUNCTION__);

			PVROSFreeKernelMem(pui8EdidData);
			return PSB_ERROR_GENERIC;
		}

		/*
		 * Byte #56,#58,#59 and #61 define the native resolution.
		 * Reference to the EDID format definition.
		 */
		ui32Width	= (((pui8EdidData[58] >> 4) << 8) | pui8EdidData[56]);
		ui32Height	= (((pui8EdidData[61] >> 4) << 8) | pui8EdidData[59]);

		/* We've got all the information we need from the EDID block so free the memory */
		PVROSFreeKernelMem(pui8EdidData);


		/* Turn off port, display plane, pipe */
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_LVDS_CTL, 0);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPBCNTR, 0);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_PIPEBCONF, 0);

		/* VGA Display Disable */
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_VGA_CTL, 0x80000000);

		/* Wait for VBLANK */
		PVROSDelayus(20000);

		/* 
		 * By default the timing registers for Pipe B are write protected when the 
		 * system boots up. If we ever want to get this working then copy the SDVO 
		 * case (as seen later in this function) but do the setup for Pipe B instead.
		 */

		ui32RegVal = 0;
		ui32RegVal = PVRPSB_DSPSIZE_WIDTH_SET(ui32RegVal, ui32Width);
		ui32RegVal = PVRPSB_DSPSIZE_HEIGHT_SET(ui32RegVal, ui32Height);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPBSIZE, ui32RegVal);

		PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPBPOS, 0);

		ui32RegVal = 0;
		ui32RegVal = PVRPSB_PIPESRC_HSIZE_SET(ui32RegVal, ui32Width);
		ui32RegVal = PVRPSB_PIPESRC_VSIZE_SET(ui32RegVal, ui32Height);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_PIPEBSRC, ui32RegVal);

		/* Enable Pipe B */
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_PIPEBCONF, 0x80000000);

		/*==================== Display B Plane Setting =================== */
		ui32RegVal = PVRPSB_DSPSTRIDE_STRIDE_SET(ui32RegVal, PVRPSB_ALIGN(ui32Width * 4, PVRPSB_DSPSTRIDE_LINEAR_MEM));
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPBSTRIDE, ui32RegVal);
		psDevInfo->sDisplayDims.ui32ByteStride = ui32RegVal;

		/* Setup the display format */
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPBCNTR, 0x99000000);
		psDevInfo->sDisplayFormat.pixelformat = PVRSRV_PIXEL_FORMAT_ARGB8888;

		PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPBLINOFF, 0);

		/* ========================= Port Control ====================== */

		/* LVDS Digital Display Port Control
		 * Border to the LVDS transmitter is disabled. (the border data would not be
		 * included in the active display data sent to the panel.
		 * Border should be used when in VGA centered (un-scaled) mode or when
		 * scaling a 4:3 source image to a wide screen panel (typical 16:9).
		 */
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_LVDS_CTL, 0xC0200300);

		psDevInfo->ui32ActivePipe		= PVRPSB_PIPE_B;
		psDevInfo->sDisplayDims.ui32Width	= ui32Width;
		psDevInfo->sDisplayDims.ui32Height	= ui32Height;
	}
	else
	{
		struct GTF_TIMINGS_DEF *psTiming = (struct GTF_TIMINGS_DEF *)&gsGtfTimings[2];
		PLL_FREQ sPllFreqInfo;
		IMG_UINT32 ui32PllCtrl;
		IMG_UINT32 ui32Fp0;
		IMG_UINT32 ui32HSyncStart;
		IMG_UINT32 ui32VSyncStart;
		IMG_UINT32 ui32HSyncEnd;
		IMG_UINT32 ui32VSyncEnd;
		IMG_UINT32 ui32HTotal;
		IMG_UINT32 ui32VTotal;

		if (PVRPSB_SDVO_CTL_ENABLE_GET(PVROSReadMMIOReg(psDevInfo, PVRPSB_SDVOB_CTL)) == 0)
		{
			printk(KERN_WARNING DRVNAME " - %s: No monitor attached (SDVO B)\n", __FUNCTION__);
		}

		if (PVRPSBSelectPLLFreq(psTiming->ui32Doc, &sPllFreqInfo, PSB_FALSE) == PSB_FALSE)
		{
			return PSB_ERROR_GENERIC;
		}

		ui32Fp0 = (sPllFreqInfo.ui32N << 16) | (sPllFreqInfo.ui32M1 << 8) | sPllFreqInfo.ui32M2;
		ui32PllCtrl = 0x54000000 | ((10 - sPllFreqInfo.ui32P2) / 5) | 
					((1 << (sPllFreqInfo.ui32P1 - 1)) << 16) | 0x030;

		/* Turn off port, display plane and pipe */
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_SDVOB_CTL, 0);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_VGA_CTL, 0x80000000);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_PIPEACONF, 0);
		/* Wait for VBLANK */
		PVROSDelayus(20000);

		PVROSWriteMMIOReg(psDevInfo, PVRPSB_DPLLA_CTL, 
				  PVROSReadMMIOReg(psDevInfo, PVRPSB_DPLLA_CTL) & 0x7FFFFFFF);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_DPLLA_CTL, ui32PllCtrl);

		/* Enable PLL */
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_FPA0, ui32Fp0);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_DPLLA_CTL, ui32PllCtrl | 0x80000000);
		PVROSDelayus(150);

		/*============== Pipe A Timing Setting (with pipe disabled) ============ */
		ui32Width	= psTiming->ui32Width;
		ui32Height	= psTiming->ui32Height;
		ui32HSyncStart	= psTiming->ui32Width + psTiming->ui32Hfront;
		ui32VSyncStart	= psTiming->ui32Height + psTiming->ui32Vfront;
		ui32HSyncEnd	= ui32HSyncStart + psTiming->ui32Hsync;
		ui32VSyncEnd	= ui32VSyncStart + psTiming->ui32Vsync;
		ui32HTotal	= psTiming->ui32Width + psTiming->ui32Hblanking;
		ui32VTotal	= psTiming->ui32Height + psTiming->ui32Vblanking;

		ui32RegVal = 0;
		ui32RegVal = PVRPSB_HTOTAL_TOTAL_SET(ui32RegVal, ui32HTotal);
		ui32RegVal = PVRPSB_HTOTAL_ACTIVE_SET(ui32RegVal, ui32Width);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_HTOTALA, ui32RegVal);

		ui32RegVal = 0;
		ui32RegVal = PVRPSB_HBLANK_END_SET(ui32RegVal, ui32HTotal);
		ui32RegVal = PVRPSB_HBLANK_START_SET(ui32RegVal, ui32Width);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_HBLANKA, ui32RegVal);

		ui32RegVal = 0;
		ui32RegVal = PVRPSB_HSYNC_END_SET(ui32RegVal, ui32HSyncEnd);
		ui32RegVal = PVRPSB_HSYNC_START_SET(ui32RegVal, ui32HSyncStart);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_HSYNCA, ui32RegVal);


		ui32RegVal = 0;
		ui32RegVal = PVRPSB_VTOTAL_TOTAL_SET(ui32RegVal, ui32VTotal);
		ui32RegVal = PVRPSB_VTOTAL_ACTIVE_SET(ui32RegVal, ui32Height);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_VTOTALA, ui32RegVal);

		ui32RegVal = 0;
		ui32RegVal = PVRPSB_VBLANK_END_SET(ui32RegVal, ui32VTotal);
		ui32RegVal = PVRPSB_VBLANK_START_SET(ui32RegVal, ui32Height);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_VBLANKA, ui32RegVal);

		ui32RegVal = 0;
		ui32RegVal = PVRPSB_VSYNC_END_SET(ui32RegVal, ui32VSyncEnd);
		ui32RegVal = PVRPSB_VSYNC_START_SET(ui32RegVal, ui32VSyncStart);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_VSYNCA, ui32RegVal);


		ui32RegVal = 0;
		ui32RegVal = PVRPSB_DSPSIZE_WIDTH_SET(ui32RegVal, ui32Width);
		ui32RegVal = PVRPSB_DSPSIZE_HEIGHT_SET(ui32RegVal, ui32Height);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPASIZE, ui32RegVal);

		PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPAPOS, 0);

		ui32RegVal = 0;
		ui32RegVal = PVRPSB_PIPESRC_HSIZE_SET(ui32RegVal, ui32Width);
		ui32RegVal = PVRPSB_PIPESRC_VSIZE_SET(ui32RegVal, ui32Height);
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_PIPEASRC, ui32RegVal);

		/* Enable Pipe A */
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_PIPEACONF, 0x80000000);

		/*==================== Display A Plane Setting =================== */
		ui32RegVal = PVRPSB_DSPSTRIDE_STRIDE_SET(ui32RegVal, PVRPSB_ALIGN(ui32Width * 4, PVRPSB_DSPSTRIDE_LINEAR_MEM));
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPASTRIDE, ui32RegVal);
		psDevInfo->sDisplayDims.ui32ByteStride = ui32RegVal;

		/* Setup the display format */
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPACNTR, 0x98000000);
		psDevInfo->sDisplayFormat.pixelformat = PVRSRV_PIXEL_FORMAT_ARGB8888;

		PVROSWriteMMIOReg(psDevInfo, PVRPSB_DSPALINOFF, 0);

		/* ========================= Port Control ====================== */
		PVROSWriteMMIOReg(psDevInfo, PVRPSB_SDVOB_CTL, 0x80000000);

		psDevInfo->ui32ActivePipe		= PVRPSB_PIPE_A;
		psDevInfo->sDisplayDims.ui32Width	= psTiming->ui32Width;
		psDevInfo->sDisplayDims.ui32Height	= psTiming->ui32Height;
	}

	return PSB_OK;
}
#endif /* defined(SUPPORT_DRI_DRM) */

IMG_VOID PVROSModeSetDeinit(PVRPSB_DEVINFO *psDevInfo)
{
#if defined(SUPPORT_DRI_DRM)
	RestoreState(psDevInfo, &psDevInfo->sInitialState);

	/* There's no point in detaching encoders from connectors since 
	   we're destroying them all anyway */
	drm_mode_config_cleanup(psDevInfo->psDrmDev);
#endif /* defined(SUPPORT_DRI_DRM) */
}

void PVROSDelayus(IMG_UINT32 ui32Timeus)
{
	udelay(ui32Timeus);
}

void *PVROSAllocKernelMem(unsigned long ulSize)
{
	return kmalloc(ulSize, GFP_KERNEL);
}

void *PVROSCallocKernelMem(unsigned long ulSize)
{
	return kcalloc(1, ulSize, GFP_KERNEL);
}

void PVROSFreeKernelMem(void *pvMem)
{
	kfree(pvMem);
}

IMG_CPU_VIRTADDR PVROSAllocKernelMemForBuffer(unsigned long ulSize, IMG_SYS_PHYADDR *psSysAddr)
{
	IMG_UINT32 ui32SizeInPages = ulSize >> PVRPSB_PAGE_SHIFT;
	IMG_CPU_VIRTADDR pvCPUVAddr;
	IMG_UINT32 ui32PageNum;

	pvCPUVAddr = __vmalloc(ulSize, 
			       GFP_KERNEL | __GFP_HIGHMEM, 
			       __pgprot((pgprot_val(PAGE_KERNEL) & ~_PAGE_CACHE_MASK) | _PAGE_CACHE_WC));

	if (pvCPUVAddr != NULL && psSysAddr != NULL)
	{
		for (ui32PageNum = 0; ui32PageNum < ui32SizeInPages; ui32PageNum++)
		{
			psSysAddr[ui32PageNum].uiAddr = VMALLOC_TO_PAGE_PHYS(pvCPUVAddr + (ui32PageNum * PVRPSB_PAGE_SIZE));
		}
	}

	return pvCPUVAddr;
}

IMG_VOID PVROSFreeKernelMemForBuffer(IMG_CPU_VIRTADDR pvCPUVAddr)
{
	vfree(pvCPUVAddr);
}

void *PVROSMapPhysAddr(IMG_SYS_PHYADDR sSysAddr, IMG_UINT32 ui32Size)
{
	void *pvAddr = ioremap(sSysAddr.uiAddr, ui32Size);
	return pvAddr;
}

void *PVROSMapPhysAddrWC(IMG_SYS_PHYADDR sSysAddr, IMG_UINT32 ui32Size)
{
	void *pvAddr = ioremap_wc(sSysAddr.uiAddr, ui32Size);
	return pvAddr;
}

void PVROSUnMapPhysAddr(void *pvAddr)
{
	iounmap(pvAddr);
}

IMG_UINT32 PVROSPciReadDWord(PVRPSB_DEVINFO *psDevInfo, IMG_UINT32 ui32Reg)
{
	IMG_UINT32 ui32RetVal = 0;

#if defined(SUPPORT_DRI_DRM)
	pci_read_config_dword(psDevInfo->psDrmDev->pdev, ui32Reg, &ui32RetVal);
#else
	pci_read_config_dword(psDevInfo->psPciDev, ui32Reg, &ui32RetVal);
#endif

	return ui32RetVal;
}

IMG_VOID PVROSPciWriteDWord(PVRPSB_DEVINFO *psDevInfo, IMG_UINT32 ui32Reg, IMG_UINT32 ui32Value)
{
#if defined(SUPPORT_DRI_DRM)
	pci_write_config_dword(psDevInfo->psDrmDev->pdev, ui32Reg, ui32Value);
#else
	pci_write_config_dword(psDevInfo->psPciDev, ui32Reg, ui32Value);
#endif
}

IMG_UINT16 PVROSPciReadWord(PVRPSB_DEVINFO *psDevInfo, IMG_UINT32 ui32Reg)
{
	IMG_UINT16 ui16RetVal = 0;

#if defined(SUPPORT_DRI_DRM)
	pci_read_config_word(psDevInfo->psDrmDev->pdev, ui32Reg, &ui16RetVal);
#else
	pci_read_config_word(psDevInfo->psPciDev, ui32Reg, &ui16RetVal);
#endif

	return ui16RetVal;
}

IMG_VOID PVROSPciWriteWord(PVRPSB_DEVINFO *psDevInfo, IMG_UINT32 ui32Reg, IMG_UINT16 ui16Value)
{
#if defined(SUPPORT_DRI_DRM)
	pci_write_config_word(psDevInfo->psDrmDev->pdev, ui32Reg, ui16Value);
#else
	pci_write_config_word(psDevInfo->psPciDev, ui32Reg, ui16Value);
#endif
}

IMG_UINT8 PVROSPciReadByte(PVRPSB_DEVINFO *psDevInfo, IMG_UINT32 ui32Reg)
{
	IMG_UINT8 ui8RetVal = 0;

#if defined(SUPPORT_DRI_DRM)
	pci_read_config_byte(psDevInfo->psDrmDev->pdev, ui32Reg, &ui8RetVal);
#else
	pci_read_config_byte(psDevInfo->psPciDev, ui32Reg, &ui8RetVal);
#endif

	return ui8RetVal;
}

IMG_VOID PVROSPciWriteByte(PVRPSB_DEVINFO *psDevInfo, IMG_UINT32 ui32Reg, IMG_UINT8 ui8Value)
{
#if defined(SUPPORT_DRI_DRM)
	pci_write_config_byte(psDevInfo->psDrmDev->pdev, ui32Reg, ui8Value);
#else
	pci_write_config_byte(psDevInfo->psPciDev, ui32Reg, ui8Value);
#endif
}

IMG_UINT32 PVROSReadIOMem(void *pvRegAddr)
{
	return ioread32(pvRegAddr);
}

void PVROSWriteIOMem(void *pvRegAddr, IMG_UINT32 ui32Value)
{
	iowrite32(ui32Value, pvRegAddr);
}

void PVROSSetIOMem(void *pvAddr, IMG_UINT8 ui8Value, IMG_UINT32 ui32Size)
{
	memset_io(pvAddr, ui8Value, ui32Size);
}

void PVROSCopyToIOMem(void *pvDstAddr, void *pvSrcAddr, IMG_UINT32 ui32Size)
{
	memcpy_toio(pvDstAddr, pvSrcAddr, ui32Size);
}

void PVROSCopyFromIOMem(void *pvDstAddr, void *pvSrcAddr, IMG_UINT32 ui32Size)
{
	memcpy_fromio(pvDstAddr, pvSrcAddr, ui32Size);
}

#if defined(PVR_DISPLAY_CONTROLLER_DRM_IOCTL)
int PVR_DRM_MAKENAME(DISPLAY_CONTROLLER, _Ioctl)(struct drm_device *psDrmDev, void *pvArg, struct drm_file *psFile)
{
	PVRPSB_DEVINFO *psDevInfo = (PVRPSB_DEVINFO *)psDrmDev->dev_private;
	uint32_t *pui32Args;
	uint32_t ui32Cmd;
	IMG_UINT32 ui32PVRDevID;

	if (pvArg == NULL)
	{
		printk(KERN_ERR DRVNAME " - %s: Missing arguments\n", __FUNCTION__);
		return -EINVAL;
	}

	pui32Args	= (uint32_t *)pvArg;
	ui32Cmd		= pui32Args[PVR_DRM_DISP_ARG_CMD];
	ui32PVRDevID	= (IMG_UINT32)pui32Args[PVR_DRM_DISP_ARG_DEV];

	if (psDevInfo == NULL || psDevInfo->ui32ID != ui32PVRDevID)
	{
		printk(KERN_ERR DRVNAME " - %s: Invalid device ID (%d)\n", __FUNCTION__, ui32PVRDevID);
		return -EINVAL;
	}

	switch (ui32Cmd)
	{
		case PVR_DRM_DISP_CMD_LEAVE_VT:
		{
			if (psDevInfo->bLeaveVT == PSB_FALSE)
			{
				SaveState(psDevInfo, &psDevInfo->sSuspendState);

				if (psDevInfo->psSwapChain != NULL)
				{
					PVRPSBFlushInternalVSyncQueue(psDevInfo);
#if defined(SYS_USING_INTERRUPTS)
					/* Do this after PVRPSBFlushInternalVSyncQueue since this will 
					   disable and re-enable VSync interrupts */
					PVRPSBDisableVSyncInterrupt(psDevInfo);
#endif
					/* Save the current buffer before flipping to the system buffer 
					   so that we can restore it when we re-enter the VT */
					psDevInfo->psSavedBuffer = psDevInfo->psCurrentBuffer;

					PVRPSBFlip(psDevInfo, psDevInfo->psSystemBuffer);
				}

				RestoreState(psDevInfo, &psDevInfo->sInitialState);
				psDevInfo->bLeaveVT = PSB_TRUE;
				psDevInfo->bEnterVTSaveState = PSB_TRUE;
			}
			break;
		}
		case PVR_DRM_DISP_CMD_ENTER_VT:
		{
			if (psDevInfo->bLeaveVT == PSB_TRUE)
			{
				psDevInfo->bLeaveVT = PSB_FALSE;

				if (psDevInfo->bEnterVTSaveState == PSB_TRUE)
				{
					SaveState(psDevInfo, &psDevInfo->sInitialState);
				}

				if (psDevInfo->psSwapChain != NULL)
				{
					if (psDevInfo->psSavedBuffer != NULL)
					{
						PVRPSBFlip(psDevInfo, psDevInfo->psSavedBuffer);
						psDevInfo->psSavedBuffer = NULL;
					}
					else
					{
						printk(KERN_ERR DRVNAME " - %s: Missing saved buffer\n", __FUNCTION__);
					}
#if defined(SYS_USING_INTERRUPTS)
					PVRPSBEnableVSyncInterrupt(psDevInfo);
#endif
				}

				RestoreState(psDevInfo, &psDevInfo->sSuspendState);
			}
			break;
		}
		default:
		{
			printk(KERN_ERR DRVNAME " - %s: Invalid command\n", __FUNCTION__);
			return -EINVAL;
		}
	}

	return 0;
}
#endif /* #if defined(PVR_DISPLAY_CONTROLLER_DRM_IOCTL) */

/*****************************************************************************
 Function Name:	PVRPSB_Init
 Description  :	Insert the driver into the kernel.

		The device major number is allocated by the kernel dynamically
		if AssignedMajorNumber is zero on entry. This means that the
		device node (nominally /dev/pvrpdp) may need to be re-made if
		the kernel varies the major number it assigns.	The number
		does seem to stay constant between runs, but I don't think
		this is guaranteed. The node is made as root on the shell
		with:
					mknod /dev/pvrpdp c ? 0

		where ? is the major number reported by the printk() - look
		at the boot log using `dmesg' to see this).

		__init places the function in a special memory section that
		the kernel frees once the function has been run. Refer also
		to module_init() macro call below.
*****************************************************************************/
#if defined(SUPPORT_DRI_DRM)
int PVR_DRM_MAKENAME(DISPLAY_CONTROLLER, _Init)(struct drm_device *psDrmDev)
#else
static int __init PVRPSB_Init(void)
#endif
{
	PVRPSB_DEVINFO *psDevInfo;

#if defined(SUPPORT_DRI_DRM)
	if (psDrmDev == NULL)
	{
		printk(KERN_ERR DRVNAME " - %s: No DRM device present\n", __FUNCTION__);
			
		return -ENODEV;
	}
#endif

	psDevInfo = PVRPSBGetDevInfo();
	if (psDevInfo == NULL)
	{
#if defined(SUPPORT_DRI_DRM)
		struct drm_connector *psConnector;
		struct drm_connector *psConnectorTemp;
#endif
		struct pci_dev *psPciDev;
		PSB_ERROR eReturn;
		int iReturn;

		/* Get and enable the PCI device so that we can poke registers */
		psPciDev = pci_get_bus_and_slot(PVRPSB_BUS_ID, PCI_DEVFN(PVRPSB_DEV_ID, PVRPSB_FUNC));
		if (psPciDev == NULL)
		{
			printk(KERN_ERR DRVNAME " - %s: pci_get_device failed\n", __FUNCTION__);
			
			return -ENODEV;
		}

		iReturn = pci_enable_device(psPciDev);
		if (iReturn != 0)
		{
			printk(KERN_ERR DRVNAME " - %s: pci_enable_device failed (%d)\n", __FUNCTION__, iReturn);

			return iReturn;
		}

		/* Allocate the device info and fill in some information */
		psDevInfo = (PVRPSB_DEVINFO *)PVROSCallocKernelMem(sizeof(PVRPSB_DEVINFO));
		if (psDevInfo == NULL)
		{
			printk(KERN_ERR DRVNAME " - %s: failed to allocate device info\n", __FUNCTION__);

			pci_disable_device(psPciDev);
			return -ENOMEM;
		}
		psDevInfo->ui32RefCount	= 0;

#if defined(SUPPORT_DRI_DRM)
		psDevInfo->psDrmDev = psDrmDev;
		psDevInfo->psDrmDev->dev_private = psDevInfo;
#else
		psDevInfo->psPciDev = psPciDev;
#endif

		/* Do the generic part of the device initialisation */
		eReturn = PVRPSBInit(psDevInfo);

#if !defined(SUPPORT_DRI_DRM)
		psDevInfo->psPciDev = NULL;
#endif

		/* To prevent possible problems with system suspend/resume, we don't
		   keep the device enabled, but rely on the fact that the SGX driver
		   will have done a pci_enable_device. */
		pci_disable_device(psPciDev);

		if (eReturn != PSB_OK)
		{
			printk(KERN_ERR DRVNAME " - %s: Can't init device (%d)\n", __FUNCTION__, eReturn);
			
			PVROSFreeKernelMem(psDevInfo);
			return -ENODEV;
		}

		/* We successfully initialised the device so store a pointer the device info for later retrieval. */
		PVRPSBSetDevInfo(psDevInfo);

#if defined(SUPPORT_DRI_DRM)
		/* Initialisation is complete so now we need to make sure the monitor(s) is/are on */
		list_for_each_entry_safe(psConnector, psConnectorTemp, &psDevInfo->psDrmDev->mode_config.connector_list, head)
		{
			drm_helper_connector_dpms(psConnector, DRM_MODE_DPMS_ON);
		}
#endif
	}
	psDevInfo->ui32RefCount++;

	return 0;
}

/*****************************************************************************
 Function Name:	PVRPSB_Cleanup
 Description  :	Remove the driver from the kernel.

		__exit places the function in a special memory section that
		the kernel frees once the function has been run. Refer also
		to module_exit() macro call below.

*****************************************************************************/
#if defined(SUPPORT_DRI_DRM)
void PVR_DRM_MAKENAME(DISPLAY_CONTROLLER, _Cleanup)(struct drm_device *psDrmDev)
#else
static void __exit PVRPSB_Cleanup(void)
#endif
{
#if defined(SUPPORT_DRI_DRM)
	PVRPSB_DEVINFO *psDevInfo = (PVRPSB_DEVINFO *)psDrmDev->dev_private;
#else
	PVRPSB_DEVINFO *psDevInfo = PVRPSBGetDevInfo();
#endif

	if (psDevInfo == NULL)
	{
		return;
	}

	psDevInfo->ui32RefCount--;
	if (psDevInfo->ui32RefCount == 0)
	{
		PSB_ERROR eReturn;
#if defined(SUPPORT_DRI_DRM)
		struct drm_connector *psConnector;
		struct drm_connector *psConnectorTemp;

		/* Turn monitors back on in case they are off */
		list_for_each_entry_safe(psConnector, psConnectorTemp, &psDevInfo->psDrmDev->mode_config.connector_list, head)
		{
			drm_helper_connector_dpms(psConnector, DRM_MODE_DPMS_ON);
		}
#endif

		eReturn = PVRPSBDeinit(psDevInfo);
		if (eReturn != PSB_OK)
		{
			printk(KERN_ERR DRVNAME " - %s: Can't deinit device (%d)\n", __FUNCTION__, eReturn);
		}

		PVRPSBSetDevInfo(NULL);
		PVROSFreeKernelMem(psDevInfo);
	}
}

/* These macro calls define the initialisation and removal functions of the
   driver. Although they are prefixed `module_', they apply when compiling
   statically as well; in both cases they define the function the kernel will
   run to start/stop the driver. */
#if !defined(SUPPORT_DRI_DRM)
module_init(PVRPSB_Init);
module_exit(PVRPSB_Cleanup);
#endif


/******************************************************************************
 End of file (poulsbo_linux.c)
******************************************************************************/
