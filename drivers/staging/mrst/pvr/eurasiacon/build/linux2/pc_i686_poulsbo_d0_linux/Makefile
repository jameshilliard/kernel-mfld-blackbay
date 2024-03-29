########################################################################### ###
#@Title         Root makefile for embedded Poulsbo.
#@Copyright     Copyright (c) Imagination Technologies Ltd. All Rights Reserved
#@License       Dual MIT/GPLv2
# 
# The contents of this file are subject to the MIT license as set out below.
# 
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
# 
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
# 
# Alternatively, the contents of this file may be used under the terms of
# the GNU General Public License Version 2 ("GPL") in which case the provisions
# of GPL are applicable instead of those above.
# 
# If you wish to allow use of your version of this file only under the terms of
# GPL, and not to allow others to use your version of this file under the terms
# of the MIT license, indicate your decision by deleting the provisions above
# and replace them with the notice and other provisions required by GPL as set
# out in the file called "GPL-COPYING" included in this distribution. If you do
# not delete the provisions above, a recipient may use your version of this file
# under the terms of either the MIT license or GPL.
# 
# This License is also included in this distribution in the file called
# "MIT-COPYING".
# 
# EXCEPT AS OTHERWISE STATED IN A NEGOTIATED AGREEMENT: (A) THE SOFTWARE IS
# PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
# BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
# PURPOSE AND NONINFRINGEMENT; AND (B) IN NO EVENT SHALL THE AUTHORS OR
# COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
# IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
# CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
### ###########################################################################
#
# Note: Use intel_linux for kontron reference platform builds.

PVR_SYSTEM := poulsbo
KERNEL_COMPONENTS := srvkm bufferclass_example

ifeq ($(SGXCORE),)
SGXCORE := 535
SGX_CORE_REV := 121
endif

DISPLAY_CONTROLLER := dc_poulsbo

include ../common/xorg_test.mk
ifeq ($(want_xorg),1)
SUPPORT_DRI_DRM := 1
SUPPORT_DRM_MODESET := 1
PVR_DISPLAY_CONTROLLER_DRM_IOCTL := 1

XORG_PVR_VIDEO := $(PVR_SYSTEM)

OPK_DEFAULT := libpvrPVR2D_FLIPWSEGL.so
OPK_FALLBACK := libpvrPVR2D_DRIWSEGL.so
else
OPK_DEFAULT := libpvrEWS_WSEGL.so
OPK_FALLBACK := libpvrPVR2D_FLIPWSEGL.so
endif

ifeq ($(SUPPORT_DRI_DRM),1)
EXTRA_PVRSRVKM_COMPONENTS += $(DISPLAY_CONTROLLER)
else
KERNEL_COMPONENTS += $(DISPLAY_CONTROLLER)
endif

# Should be last
include ../config/core.mk
include ../common/xorg.mk
include ../common/dridrm.mk
include ../common/opencl.mk

ifeq ($(SUPPORT_DRI_DRM),1)
$(eval $(call TunableKernelConfigC,PVRPSB_WIDTH,$(PVRPSB_WIDTH)))
$(eval $(call TunableKernelConfigC,PVRPSB_HEIGHT,$(PVRPSB_HEIGHT)))
$(eval $(call TunableKernelConfigC,PVRPSB_VREFRESH,$(PVRPSB_VREFRESH)))
endif
