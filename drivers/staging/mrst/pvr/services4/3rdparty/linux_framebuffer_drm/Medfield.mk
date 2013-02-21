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

MRST_SOURCE_DIR := drivers/staging/mrst

ccflags-y += \
 -Iinclude/drm \
 -I$(TOP)/services4/include/env/linux \
 -I$(TOP)/services4/3rdparty/linux_framebuffer_drm \
 -DPVR_MRST_FB_SET_PAR_ON_INIT \
 -DCONFIG_DRM_INTEL_MID \
 -DCONFIG_DRM_MDFLD \
 -DCONFIG_MDFLD_DSI_DBI \
 -DCONFIG_MDFD_COMMAND_MODE_2

ifeq ($(MRST_DRIVER_SOURCE),)
ccflags-y += \
 -I$(OUT)/target/kbuild/external/$(MRST_SOURCE_DIR) \
 -I$(OUT)/target/kbuild/external/$(MRST_SOURCE_DIR)/drv \
 -I$(OUT)/target/kbuild/external/$(MRST_SOURCE_DIR)/imgv
else
ccflags-y += \
 -I$(MRST_DRIVER_SOURCE)/$(MRST_SOURCE_DIR) \
 -I$(MRST_DRIVER_SOURCE)/$(MRST_SOURCE_DIR)/drv \
 -I$(MRST_DRIVER_SOURCE)/$(MRST_SOURCE_DIR)/imgv
endif

medfield_drm-y += \
 services4/srvkm/env/linux/pvr_drm.o \
 services4/3rdparty/linux_framebuffer_drm/drmlfb_displayclass.o \
 services4/3rdparty/linux_framebuffer_drm/drmlfb_linux.o \
 services4/system/$(PVR_SYSTEM)/sys_pvr_drm_export.o

medfield_drm-y += \
 external/$(MRST_SOURCE_DIR)/drv/psb_bl.medfield.o \
 external/$(MRST_SOURCE_DIR)/drv/psb_dpst.medfield.o \
 external/$(MRST_SOURCE_DIR)/drv/psb_drv.medfield.o \
 external/$(MRST_SOURCE_DIR)/drv/psb_fb.medfield.o \
 external/$(MRST_SOURCE_DIR)/drv/psb_gtt.medfield.o \
 external/$(MRST_SOURCE_DIR)/drv/psb_hotplug.medfield.o \
 external/$(MRST_SOURCE_DIR)/drv/psb_intel_bios.medfield.o \
 external/$(MRST_SOURCE_DIR)/drv/psb_intel_display.medfield.o \
 external/$(MRST_SOURCE_DIR)/drv/psb_intel_i2c.medfield.o \
 external/$(MRST_SOURCE_DIR)/drv/psb_intel_lvds.medfield.o \
 external/$(MRST_SOURCE_DIR)/drv/psb_intel_modes.medfield.o \
 external/$(MRST_SOURCE_DIR)/drv/psb_intel_sdvo.medfield.o \
 external/$(MRST_SOURCE_DIR)/drv/psb_intel_hdmi.medfield.o \
 external/$(MRST_SOURCE_DIR)/drv/psb_intel_hdmi_i2c.medfield.o \
 external/$(MRST_SOURCE_DIR)/drv/psb_reset.medfield.o \
 external/$(MRST_SOURCE_DIR)/drv/psb_schedule.medfield.o \
 external/$(MRST_SOURCE_DIR)/drv/psb_sgx.medfield.o \
 external/$(MRST_SOURCE_DIR)/drv/psb_socket.medfield.o \
 external/$(MRST_SOURCE_DIR)/drv/psb_pvr_glue.medfield.o \
 external/$(MRST_SOURCE_DIR)/drv/psb_umevents.medfield.o \
 external/$(MRST_SOURCE_DIR)/drv/psb_intel_dsi.medfield.o \
 external/$(MRST_SOURCE_DIR)/drv/psb_intel_dsi_aava.medfield.o \
 external/$(MRST_SOURCE_DIR)/drv/mdfld_dsi_dbi.medfield.o \
 external/$(MRST_SOURCE_DIR)/drv/mdfld_dsi_dpi.medfield.o \
 external/$(MRST_SOURCE_DIR)/drv/mdfld_dsi_output.medfield.o \
 external/$(MRST_SOURCE_DIR)/drv/psb_powermgmt.medfield.o \
 external/$(MRST_SOURCE_DIR)/drv/psb_irq.medfield.o

medfield_drm-y += \
 external/$(MRST_SOURCE_DIR)/imgv/lnc_topaz.medfield.o \
 external/$(MRST_SOURCE_DIR)/imgv/lnc_topazinit.medfield.o \
 external/$(MRST_SOURCE_DIR)/imgv/pnw_topaz.medfield.o \
 external/$(MRST_SOURCE_DIR)/imgv/pnw_topazinit.medfield.o \
 external/$(MRST_SOURCE_DIR)/imgv/psb_buffer.medfield.o \
 external/$(MRST_SOURCE_DIR)/imgv/psb_fence.medfield.o \
 external/$(MRST_SOURCE_DIR)/imgv/psb_mmu.medfield.o \
 external/$(MRST_SOURCE_DIR)/imgv/psb_msvdx.medfield.o \
 external/$(MRST_SOURCE_DIR)/imgv/psb_msvdxinit.medfield.o \
 external/$(MRST_SOURCE_DIR)/imgv/psb_ttm_glue.medfield.o \
 external/$(MRST_SOURCE_DIR)/imgv/psb_ttm_fence.medfield.o \
 external/$(MRST_SOURCE_DIR)/imgv/psb_ttm_fence_user.medfield.o \
 external/$(MRST_SOURCE_DIR)/imgv/psb_ttm_placement_user.medfield.o \

medfield_drm-y += \
 external/$(MRST_SOURCE_DIR)/ttm/ttm_agp_backend.o \
 external/$(MRST_SOURCE_DIR)/ttm/ttm_memory.o \
 external/$(MRST_SOURCE_DIR)/ttm/ttm_tt.o \
 external/$(MRST_SOURCE_DIR)/ttm/ttm_bo.o \
 external/$(MRST_SOURCE_DIR)/ttm/ttm_bo_util.o \
 external/$(MRST_SOURCE_DIR)/ttm/ttm_bo_vm.o \
 external/$(MRST_SOURCE_DIR)/ttm/ttm_module.o \
 external/$(MRST_SOURCE_DIR)/ttm/ttm_global.o \
 external/$(MRST_SOURCE_DIR)/ttm/ttm_object.o \
 external/$(MRST_SOURCE_DIR)/ttm/ttm_lock.o \
 external/$(MRST_SOURCE_DIR)/ttm/ttm_execbuf_util.o \
 external/$(MRST_SOURCE_DIR)/ttm/ttm_page_alloc.o

medfield_drm-y += \
 external/$(MRST_SOURCE_DIR)/drm_global.o
