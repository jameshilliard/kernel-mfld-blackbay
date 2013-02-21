########################################################################### ###
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

MRST_SOURCE_DIR := drivers/gpu/drm/mrst/drv

ccflags-y += \
 -Iinclude/drm \
 -I$(TOP)/services4/include/env/linux \
 -I$(TOP)/services4/3rdparty/linux_framebuffer_drm \
 -DPVR_MRST_FB_SET_PAR_ON_INIT

ifeq ($(MRST_DRIVER_SOURCE),)
ccflags-y += -I$(OUT)/target/kbuild/external/$(MRST_SOURCE_DIR)
else
ccflags-y += -I$(MRST_DRIVER_SOURCE)/$(MRST_SOURCE_DIR)
endif

poulsbo_drm-y += \
 services4/srvkm/env/linux/pvr_drm.o \
 services4/3rdparty/linux_framebuffer_drm/drmlfb_displayclass.o \
 services4/3rdparty/linux_framebuffer_drm/drmlfb_linux.o \
 services4/system/$(PVR_SYSTEM)/sys_pvr_drm_export.o

poulsbo_drm-y += \
 external/$(MRST_SOURCE_DIR)/psb_bl.o \
 external/$(MRST_SOURCE_DIR)/psb_dpst.o \
 external/$(MRST_SOURCE_DIR)/psb_drv.o \
 external/$(MRST_SOURCE_DIR)/psb_fb.o \
 external/$(MRST_SOURCE_DIR)/psb_gtt.o \
 external/$(MRST_SOURCE_DIR)/psb_hotplug.o \
 external/$(MRST_SOURCE_DIR)/psb_intel_bios.o \
 external/$(MRST_SOURCE_DIR)/psb_intel_display.o \
 external/$(MRST_SOURCE_DIR)/psb_intel_dsi.o \
 external/$(MRST_SOURCE_DIR)/psb_intel_i2c.o \
 external/$(MRST_SOURCE_DIR)/psb_intel_lvds.o \
 external/$(MRST_SOURCE_DIR)/psb_intel_modes.o \
 external/$(MRST_SOURCE_DIR)/psb_intel_sdvo.o \
 external/$(MRST_SOURCE_DIR)/psb_irq.o \
 external/$(MRST_SOURCE_DIR)/psb_powermgmt.o \
 external/$(MRST_SOURCE_DIR)/psb_socket.o \
 external/$(MRST_SOURCE_DIR)/psb_pvr_glue.o \
 external/$(MRST_SOURCE_DIR)/psb_umevents.o
