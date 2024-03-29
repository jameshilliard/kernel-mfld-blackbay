########################################################################### ###
#@Title         Root makefile for No HW Linux.  Builds everything else.
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

PVR_SYSTEM := sgx_nohw

include ../kernel_version.mk

BUFFERCLASS_MODULE ?= bufferclass_example
KERNEL_COMPONENTS := srvkm $(BUFFERCLASS_MODULE)

DISPLAY_CONTROLLER := dcnohw

OPK_DEFAULT := libpvrPVR2D_FLIPWSEGL.so

PDUMP ?= 1
NO_HARDWARE := 1

ifneq ($(SGXCORE),520)
SGX_FAST_DPM_INIT ?= 1
endif

SUPPORT_SGX_HWPERF ?= 0

DC_NOHW_WIDTH ?= 640
DC_NOHW_HEIGHT ?= 480

include ../common/xorg_test.mk
ifeq ($(want_xorg),1)
SUPPORT_DRI_DRM := 1
OPK_FALLBACK := libpvrPVR2D_DRIWSEGL.so
else
OPK_FALLBACK := libpvrEWS_WSEGL.so
endif

ifeq ($(SUPPORT_DRI_DRM),1)
EXTRA_PVRSRVKM_COMPONENTS += dc_nohw
ifeq ($(call kernel-version-at-least,2,6,39),true)
# For the benefit of DRM, the SGX driver will create a dummy platform
# device to represent SGX.
LDM_PCI := 0
LDM_PLATFORM := 1
PVR_DRI_DRM_PLATFORM_DEV := 1
PVR_DRI_DRM_STATIC_BUS_ID := 1
ifeq ($(call kernel-version-at-least,3,1),true)
PVR_DRI_DRM_DEV_BUS_ID := "\"platform:sgxnohw:-1\""
endif
else
# Build a DRM module that doesn't require a real SGX device to manage.
# DRM needs to be removed from the Linux kernel configuration for this to
# work.
PVR_DRI_DRM_NOT_PCI := 1
KERNEL_COMPONENTS += linux_drm
EXTRA_KBUILD_SOURCE := $(KERNELDIR)
endif
else
KERNEL_COMPONENTS += dc_nohw
endif

# Should be last
include ../config/core.mk
include ../common/xorg.mk
include ../common/dridrm.mk
include ../common/opencl.mk
$(eval $(call KernelConfigC,DC_NOHW_BUFFER_WIDTH,$(DC_NOHW_WIDTH)))
$(eval $(call KernelConfigC,DC_NOHW_BUFFER_HEIGHT,$(DC_NOHW_HEIGHT)))
