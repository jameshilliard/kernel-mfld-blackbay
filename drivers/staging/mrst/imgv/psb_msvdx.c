/**************************************************************************
 * MSVDX I/O operations and IRQ handling
 *
 * Copyright (c) 2007 Intel Corporation, Hillsboro, OR, USA
 * Copyright (c) Imagination Technologies Limited, UK
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 **************************************************************************/

#include <drm/drmP.h>
#include "psb_drm.h"
#include "psb_drv.h"
#include "psb_msvdx.h"
#include "psb_msvdx_ec.h"
#include "pnw_topaz.h"
#include "psb_powermgmt.h"
#include <linux/io.h>
#include <linux/delay.h>

#ifndef list_first_entry
#define list_first_entry(ptr, type, member) \
	list_entry((ptr)->next, type, member)
#endif

static int ied_enabled;

static int psb_msvdx_send(struct drm_device *dev, void *cmd,
			  unsigned long cmd_size);

static void psb_msvdx_set_tile(struct drm_device *dev,
				unsigned long msvdx_tile);

static int psb_msvdx_dequeue_send(struct drm_device *dev)
{
	struct drm_psb_private *dev_priv = dev->dev_private;
	struct psb_msvdx_cmd_queue *msvdx_cmd = NULL;
	int ret = 0;
	struct msvdx_private *msvdx_priv = dev_priv->msvdx_private;
	unsigned long irq_flags;

	spin_lock_irqsave(&msvdx_priv->msvdx_lock, irq_flags);
	if (list_empty(&msvdx_priv->msvdx_queue)) {
		PSB_DEBUG_GENERAL("MSVDXQUE: msvdx list empty.\n");
		msvdx_priv->msvdx_busy = 0;
		spin_unlock_irqrestore(&msvdx_priv->msvdx_lock, irq_flags);
		return -EINVAL;
	}

	msvdx_cmd = list_first_entry(&msvdx_priv->msvdx_queue,
				     struct psb_msvdx_cmd_queue, head);
	list_del(&msvdx_cmd->head);
	spin_unlock_irqrestore(&msvdx_priv->msvdx_lock, irq_flags);

	PSB_DEBUG_GENERAL("MSVDXQUE: Queue has id %08x\n", msvdx_cmd->sequence);
	if (IS_MSVDX_MEM_TILE(dev) && drm_psb_msvdx_tiling)
		psb_msvdx_set_tile(dev, msvdx_cmd->msvdx_tile);

	if (msvdx_cmd->host_be_opp_enabled) {
		psb_msvdx_update_frame_info(msvdx_priv,
			msvdx_cmd->cmd + msvdx_cmd->deblock_cmd_offset);
	}
	psb_msvdx_backup_cmd(msvdx_priv,
			msvdx_cmd->cmd,
			msvdx_cmd->cmd_size,
			msvdx_cmd->deblock_cmd_offset);
	ret = psb_msvdx_send(dev, msvdx_cmd->cmd, msvdx_cmd->cmd_size);
	if (ret) {
		DRM_ERROR("MSVDXQUE: psb_msvdx_send failed\n");
		ret = -EINVAL;
	}
	kfree(msvdx_cmd->cmd);
	kfree(msvdx_cmd);

	return ret;
}

static int psb_msvdx_map_command(struct drm_device *dev,
				 struct ttm_buffer_object *cmd_buffer,
				 unsigned long cmd_offset, unsigned long cmd_size,
				 void **msvdx_cmd, uint32_t sequence, int copy_cmd)
{
	struct drm_psb_private *dev_priv = dev->dev_private;
	struct msvdx_private *msvdx_priv = dev_priv->msvdx_private;
	int ret = 0;
	unsigned long cmd_page_offset = cmd_offset & ~PAGE_MASK;
	unsigned long cmd_size_remaining;
	struct ttm_bo_kmap_obj cmd_kmap, regio_kmap;
	void *cmd, *cmd_copy, *cmd_start;
	bool is_iomem;
	int i;
	struct HOST_BE_OPP_PARAMS * oppParam;
	drm_psb_msvdx_frame_info_t * current_frame = NULL;
	int first_empty = -1;


	/* command buffers may not exceed page boundary */
	if (cmd_size + cmd_page_offset > PAGE_SIZE)
		return -EINVAL;

	ret = ttm_bo_kmap(cmd_buffer, cmd_offset >> PAGE_SHIFT, 1, &cmd_kmap);
	if (ret) {
		DRM_ERROR("MSVDXQUE:ret:%d\n", ret);
		return ret;
	}

	cmd_start = (void *)ttm_kmap_obj_virtual(&cmd_kmap, &is_iomem)
		    + cmd_page_offset;
	cmd = cmd_start;
	cmd_size_remaining = cmd_size;

	msvdx_priv->host_be_opp_enabled = 0;
	msvdx_priv->deblock_cmd_offset = PSB_MSVDX_INVALID_OFFSET;

	while (cmd_size_remaining > 0) {
		uint32_t cur_cmd_size = MEMIO_READ_FIELD(cmd, FWRK_GENMSG_SIZE);
		uint32_t cur_cmd_id = MEMIO_READ_FIELD(cmd, FWRK_GENMSG_ID);
		uint32_t mmu_ptd = 0, msvdx_mmu_invalid = 0;
		struct psb_msvdx_deblock_queue *msvdx_deblock;
		unsigned long irq_flags;

#if 0
		PSB_DEBUG_GENERAL("cmd start at %08x cur_cmd_size = %d"
				  " cur_cmd_id = %02x fence = %08x\n",
				  (uint32_t) cmd, cur_cmd_size, cur_cmd_id, sequence);
#else
		PSB_DEBUG_GENERAL("cmd start at %08x cur_cmd_size = %d,"
			" cmd_size_remaining = %d"
			" cur_cmd_id = %02x fence = %08x\n",
			(uint32_t) cmd, cur_cmd_size, cmd_size_remaining,
			cur_cmd_id, sequence);
#endif
		if ((cur_cmd_size % sizeof(uint32_t))
		    || (cur_cmd_size > cmd_size_remaining)) {
			ret = -EINVAL;
			DRM_ERROR("MSVDX: ret:%d\n", ret);
			goto out;
		}

		switch (cur_cmd_id) {
		case VA_MSGID_RENDER:
			PSB_DEBUG_MSVDX("MSVDX_DEBUG: send render message. \n");

			/* Fence ID */
			if (IS_MDFLD(dev) && IS_FW_UPDATED)
				MEMIO_WRITE_FIELD(cmd, FW_VA_DECODE_MSG_ID, sequence);
			else
				MEMIO_WRITE_FIELD(cmd, FW_VA_RENDER_FENCE_VALUE, sequence);
			mmu_ptd = psb_get_default_pd_addr(dev_priv->mmu);
			msvdx_mmu_invalid = atomic_cmpxchg(&dev_priv->msvdx_mmu_invaldc,
							   1, 0);
			if (msvdx_mmu_invalid == 1) {
				if (!(IS_MDFLD(dev) && IS_FW_UPDATED))
					mmu_ptd |= 1;
				else 
				{
					uint32_t flags;
					flags = MEMIO_READ_FIELD(cmd, FW_DEVA_DECODE_FLAGS);
					flags |= FW_DEVA_INVALIDATE_MMU;
					MEMIO_WRITE_FIELD(cmd, FW_DEVA_DECODE_FLAGS, flags);
					psb_gl3_global_invalidation(dev);
				}

				PSB_DEBUG_GENERAL("MSVDX:Set MMU invalidate\n");
			}

			/* PTD */
			if (IS_MDFLD(dev) && IS_FW_UPDATED) {
				uint32_t context_id;
				context_id = MEMIO_READ_FIELD(cmd, FW_VA_DECODE_MMUPTD);
				mmu_ptd = mmu_ptd | (context_id & 0xff);
				MEMIO_WRITE_FIELD(cmd, FW_VA_DECODE_MMUPTD, mmu_ptd);
			} else
				MEMIO_WRITE_FIELD(cmd, FW_VA_RENDER_MMUPTD, mmu_ptd);
			break;

		case VA_MSGID_OOLD:
			MEMIO_WRITE_FIELD(cmd, FW_DXVA_OOLD_FENCE_VALUE,
					  sequence);
			mmu_ptd = psb_get_default_pd_addr(dev_priv->mmu);
			msvdx_mmu_invalid = atomic_cmpxchg(&dev_priv->msvdx_mmu_invaldc,
							   1, 0);
			if (msvdx_mmu_invalid == 1) {
				mmu_ptd |= 1;
				PSB_DEBUG_GENERAL("MSVDX:Set MMU invalidate\n");
			}

			/* PTD */
			MEMIO_WRITE_FIELD(cmd, FW_DXVA_OOLD_MMUPTD, mmu_ptd);

			PSB_DEBUG_GENERAL("MSVDX:Get oold cmd\n");

			break;
		case VA_MSGID_HOST_BE_OPP_MFLD:
			msvdx_priv->host_be_opp_enabled = 1;
			msvdx_priv->deblock_cmd_offset =
					cmd_size - cmd_size_remaining;
		case VA_MSGID_OOLD_MFLD:
		case VA_MSGID_DEBLOCK_MFLD: {
			FW_VA_DEBLOCK_MSG * deblock_msg;

			PSB_DEBUG_GENERAL("MSVDX:Get deblock cmd for medfield\n");

			deblock_msg = (FW_VA_DEBLOCK_MSG *)cmd;

			mmu_ptd = psb_get_default_pd_addr(dev_priv->mmu);
			msvdx_mmu_invalid = atomic_cmpxchg(&dev_priv->msvdx_mmu_invaldc,
							   1, 0);
			if (msvdx_mmu_invalid == 1) {
				uint32_t flags;
				flags = deblock_msg->flags;
				flags |= FW_DEVA_INVALIDATE_MMU;
				deblock_msg->flags = flags;

				PSB_DEBUG_GENERAL("MSVDX:Set MMU invalidate\n");
			}

			/* patch to right cmd type */
			deblock_msg->header.bits.msg_type =
					cur_cmd_id -
					VA_MSGID_DEBLOCK_MFLD +
					VA_MSGID_DEBLOCK;

			deblock_msg->header.bits.msg_fence = (uint16_t)(sequence & 0xffff);
			deblock_msg->mmu_context.bits.mmu_ptd = (mmu_ptd >> 8);

			cmd += (sizeof(FW_VA_DEBLOCK_MSG) - cur_cmd_size);
			cmd_size_remaining -= (sizeof(FW_VA_DEBLOCK_MSG) -
					       cur_cmd_size);
		}
		break;

		case VA_MSGID_DEBLOCK:
			msvdx_priv->deblock_enabled = 1;
			/* Fence ID */
			MEMIO_WRITE_FIELD(cmd, FW_DXVA_DEBLOCK_FENCE_VALUE,
					  sequence);
			mmu_ptd = psb_get_default_pd_addr(dev_priv->mmu);
			msvdx_mmu_invalid = atomic_cmpxchg(&dev_priv->msvdx_mmu_invaldc,
							   1, 0);
			if (msvdx_mmu_invalid == 1) {
				mmu_ptd |= 1;
				PSB_DEBUG_GENERAL("MSVDX:Set MMU invalidate\n");
			}

			/* PTD */
			MEMIO_WRITE_FIELD(cmd,
					  FW_DXVA_DEBLOCK_MMUPTD,
					  mmu_ptd);

			/* printk("Got deblock msg\n"); */
			/* Deblock message is followed by 32 */
			/* bytes of deblock params */
			msvdx_deblock = kmalloc(
						sizeof(struct psb_msvdx_deblock_queue),
						GFP_KERNEL);

			if (msvdx_deblock == NULL) {
				DRM_ERROR("DEBLOCK QUE: Out of memory...\n");
				ret =  -ENOMEM;
				goto out;
			}

			memcpy(&msvdx_deblock->dbParams, cmd + 16, sizeof(struct DEBLOCKPARAMS));

			ret = ttm_bo_kmap(
				      (struct ttm_buffer_object *)
				      msvdx_deblock->dbParams.handle,
				      0,
				      (msvdx_deblock->dbParams.buffer_size +
				       PAGE_SIZE - 1) >> PAGE_SHIFT,
				      &regio_kmap);

			/* printk("deblock regio buffer size is 0x%x\n",
			   msvdx_deblock->dbParams.buffer_size); */

			if (likely(!ret)) {
				msvdx_deblock->dbParams.pPicparams = kmalloc(
						msvdx_deblock->dbParams.buffer_size,
						GFP_KERNEL);

				if (msvdx_deblock->dbParams.pPicparams != NULL)
					memcpy(
						msvdx_deblock->dbParams.pPicparams,
						regio_kmap.virtual,
						msvdx_deblock->dbParams.buffer_size);
				ttm_bo_kunmap(&regio_kmap);
			}
			spin_lock_irqsave(&msvdx_priv->msvdx_lock, irq_flags);
			list_add_tail(&msvdx_deblock->head,
				      &msvdx_priv->deblock_queue);
			spin_unlock_irqrestore(&msvdx_priv->msvdx_lock,
					       irq_flags);

			cmd += sizeof(struct DEBLOCKPARAMS);
			cmd_size_remaining -= sizeof(struct DEBLOCKPARAMS);
			break;

		case VA_MSGID_HOST_BE_OPP:

			PSB_DEBUG_MSVDX("MSVDX_DEBUG: send host_be_opp message. \n");

			msvdx_priv->host_be_opp_enabled = 1;
			/* Fence ID */
			MEMIO_WRITE_FIELD(cmd, FW_VA_HOST_BE_OPP_FENCE_VALUE,
					  sequence);
			mmu_ptd = psb_get_default_pd_addr(dev_priv->mmu);
			msvdx_mmu_invalid = atomic_cmpxchg(&dev_priv->msvdx_mmu_invaldc,
							   1, 0);
			if (msvdx_mmu_invalid == 1) {
				mmu_ptd |= 1;
				PSB_DEBUG_GENERAL("MSVDX:Set MMU invalidate\n");
			}

			/* PTD */
			MEMIO_WRITE_FIELD(cmd,
					  FW_VA_HOST_BE_OPP_MMUPTD,
					  mmu_ptd);

			/* HostBeOpp message is followed by 32 bytes of host_be_opp params */
			oppParam = kmalloc(
					   sizeof(struct HOST_BE_OPP_PARAMS),
					   GFP_KERNEL);

			if (oppParam == NULL) {
				DRM_ERROR("DEBLOCK QUE: Out of memory...\n");
				ret =  -ENOMEM;
				goto out;
			}

			memcpy(oppParam, cmd + 16, sizeof(struct HOST_BE_OPP_PARAMS));
			/*get the right frame_info struct for current surface*/
			for (i = 0; i < MAX_DECODE_BUFFERS; i++) {
				if (msvdx_priv->frame_info[i].handle == oppParam->handle) {
					current_frame = &(msvdx_priv->frame_info[i]);
					break;
				}
				if ((first_empty == -1) && (msvdx_priv->frame_info[i].handle  == 0))
					first_empty = i;
			}

			/*if didn't find the struct for current surface, use the earliest empty one*/
			if (!current_frame) {
				if (first_empty == -1) {
					DRM_ERROR("failed find the struct for current surface and also there is no empty one.\n");
					ret = -EFAULT;
					goto out;
				}
				current_frame = &(msvdx_priv->frame_info[first_empty]);
			}

			memset(current_frame, 0, sizeof(drm_psb_msvdx_frame_info_t));
			current_frame->handle = oppParam->handle;
			current_frame->buffer_stride = oppParam->buffer_stride;
			current_frame->buffer_size = oppParam->buffer_size;
			current_frame->picture_width_mb = oppParam->picture_width_mb;
			current_frame->size_mb = oppParam->size_mb;
			current_frame->fence = sequence;


			cmd += sizeof(struct HOST_BE_OPP_PARAMS);
			cmd_size_remaining -= sizeof(struct HOST_BE_OPP_PARAMS);

			break;

		default:
			/* Msg not supported */
			ret = -EINVAL;
			PSB_DEBUG_GENERAL("MSVDX: ret:%d\n", ret);
			goto out;
		}

		cmd += cur_cmd_size;
		cmd_size_remaining -= cur_cmd_size;

		if (((sequence++) & 0xf) == 0xf) {
			ret = -EINVAL;
			PSB_DEBUG_GENERAL("MSVDX: too many cmds, abort\n");
			goto out;
		}
	}

	if (copy_cmd) {
		PSB_DEBUG_GENERAL("MSVDXQUE:copying command\n");

		cmd_copy = kzalloc(cmd_size, GFP_KERNEL);
		if (cmd_copy == NULL) {
			ret = -ENOMEM;
			DRM_ERROR("MSVDX: fail to callc,ret=:%d\n", ret);
			goto out;
		}
		memcpy(cmd_copy, cmd_start, cmd_size);
		*msvdx_cmd = cmd_copy;
	} else {
		PSB_DEBUG_GENERAL("MSVDXQUE:did NOT copy command\n");
		if (IS_MSVDX_MEM_TILE(dev) && drm_psb_msvdx_tiling) {
			unsigned long msvdx_tile =
				((dev_priv->msvdx_ctx->ctx_type >> 16) & 0xff);
			psb_msvdx_set_tile(dev, msvdx_tile);
		}
		if (msvdx_priv->host_be_opp_enabled) {
			psb_msvdx_update_frame_info(msvdx_priv,
				cmd_start + msvdx_priv->deblock_cmd_offset);
		}
		psb_msvdx_backup_cmd(msvdx_priv,
				cmd_start,
				cmd_size,
				msvdx_priv->deblock_cmd_offset);
		ret = psb_msvdx_send(dev, cmd_start, cmd_size);
		if (ret) {
			DRM_ERROR("MSVDXQUE: psb_msvdx_send failed\n");
			ret = -EINVAL;
		}
	}

out:
	ttm_bo_kunmap(&cmd_kmap);

	return ret;
}

int psb_submit_video_cmdbuf(struct drm_device *dev,
			    struct ttm_buffer_object *cmd_buffer,
			    unsigned long cmd_offset, unsigned long cmd_size,
			    struct ttm_fence_object *fence)
{
	struct drm_psb_private *dev_priv = dev->dev_private;
	uint32_t sequence =  (dev_priv->sequence[PSB_ENGINE_VIDEO] << 4);
	unsigned long irq_flags;
	int ret = 0;
	struct msvdx_private *msvdx_priv = dev_priv->msvdx_private;
	int offset = 0;
	/* psb_schedule_watchdog(dev_priv); */

	spin_lock_irqsave(&msvdx_priv->msvdx_lock, irq_flags);
	if (!IS_D0(dev)) {
		/* expected msvdx_needs_reset is set after previous session exited
		 * but msvdx_hw_busy is always 1, and caused powerdown not excuted
		 * so reload the firmware for every new context
		 */
		if (IS_MRST(dev) && (dev_priv->last_msvdx_ctx == NULL))
			msvdx_priv->msvdx_needs_reset = 1; /* re-load firmware */

		if (IS_MRST(dev) && dev_priv->msvdx_ctx && dev_priv->last_msvdx_ctx) {
			uint32_t from_profile, to_profile;

			from_profile =
				(dev_priv->last_msvdx_ctx->ctx_type >> 8) &
				0xff;
			to_profile =
				(dev_priv->msvdx_ctx->ctx_type >> 8) & 0xff;

			/* not the same profile, and one of them is H264 constrained BP
			 * which needs Error Concealment firmware
			 */
			if ((from_profile != to_profile) &&
			    ((from_profile == VAProfileH264ConstrainedBaseline) ||
			     (to_profile == VAProfileH264ConstrainedBaseline))) {
				PSB_DEBUG_INIT("MSVDX: firmware switching needed\n");
				msvdx_priv->msvdx_needs_reset = 1; /* re-load firmware */
			}
		}
	}
	dev_priv->last_msvdx_ctx = dev_priv->msvdx_ctx;

	if (msvdx_priv->msvdx_needs_reset) {
		spin_unlock_irqrestore(&msvdx_priv->msvdx_lock, irq_flags);
		PSB_DEBUG_GENERAL("MSVDX: will reset msvdx\n");
		if (!IS_D0(dev)) {
			if (psb_msvdx_reset(dev_priv)) {
				ret = -EBUSY;
				DRM_ERROR("MSVDX: Reset failed\n");
				return ret;
			}
		}
		msvdx_priv->msvdx_needs_reset = 0;
		msvdx_priv->msvdx_busy = 0;

		psb_msvdx_init(dev);

		/* restore vec local mem if needed */
		if (msvdx_priv->vec_local_mem_saved) {
			for (offset = 0; offset < VEC_LOCAL_MEM_BYTE_SIZE / 4; ++offset)
				PSB_WMSVDX32(msvdx_priv->vec_local_mem_data[offset],
					     VEC_LOCAL_MEM_OFFSET + offset * 4);

			msvdx_priv->vec_local_mem_saved = 0;
		}

		spin_lock_irqsave(&msvdx_priv->msvdx_lock, irq_flags);
	}

	if (!msvdx_priv->msvdx_fw_loaded) {
		spin_unlock_irqrestore(&msvdx_priv->msvdx_lock, irq_flags);
		PSB_DEBUG_GENERAL("MSVDX:reload FW to MTX\n");

		ret = psb_setup_fw(dev);
		if (ret) {
			DRM_ERROR("MSVDX:fail to load FW\n");
			/* FIXME: find a proper return value */
			return -EFAULT;
		}
		msvdx_priv->msvdx_fw_loaded = 1;

		PSB_DEBUG_GENERAL("MSVDX: load firmware successfully\n");
		spin_lock_irqsave(&msvdx_priv->msvdx_lock, irq_flags);
	}

	if (!msvdx_priv->msvdx_busy) {
		msvdx_priv->msvdx_busy = 1;
		spin_unlock_irqrestore(&msvdx_priv->msvdx_lock, irq_flags);
		PSB_DEBUG_GENERAL("MSVDX: commit command to HW,seq=0x%08x\n",
				  sequence);
		ret = psb_msvdx_map_command(dev, cmd_buffer, cmd_offset,
					    cmd_size, NULL, sequence, 0);
		if (ret) {
			DRM_ERROR("MSVDXQUE: Failed to extract cmd\n");
			return ret;
		}
	} else {
		struct psb_msvdx_cmd_queue *msvdx_cmd;
		void *cmd = NULL;

		spin_unlock_irqrestore(&msvdx_priv->msvdx_lock, irq_flags);
		/* queue the command to be sent when the h/w is ready */
		PSB_DEBUG_GENERAL("MSVDXQUE: queueing sequence:%08x..\n",
				  sequence);
		msvdx_cmd = kzalloc(sizeof(struct psb_msvdx_cmd_queue),
				    GFP_KERNEL);
		if (msvdx_cmd == NULL) {
			DRM_ERROR("MSVDXQUE: Out of memory...\n");
			return -ENOMEM;
		}

		ret = psb_msvdx_map_command(dev, cmd_buffer, cmd_offset,
					    cmd_size, &cmd, sequence, 1);
		if (ret) {
			DRM_ERROR("MSVDXQUE: Failed to extract cmd\n");
			kfree(msvdx_cmd
			     );
			return ret;
		}
		msvdx_cmd->cmd = cmd;
		msvdx_cmd->cmd_size = cmd_size;
		msvdx_cmd->sequence = sequence;
		msvdx_cmd->msvdx_tile =
			((dev_priv->msvdx_ctx->ctx_type >> 16) & 0xff);
		msvdx_cmd->deblock_cmd_offset =
			msvdx_priv->deblock_cmd_offset;
		msvdx_cmd->host_be_opp_enabled =
			msvdx_priv->host_be_opp_enabled;
		spin_lock_irqsave(&msvdx_priv->msvdx_lock, irq_flags);
		list_add_tail(&msvdx_cmd->head, &msvdx_priv->msvdx_queue);
		spin_unlock_irqrestore(&msvdx_priv->msvdx_lock, irq_flags);
		if (!msvdx_priv->msvdx_busy) {
			msvdx_priv->msvdx_busy = 1;
			PSB_DEBUG_GENERAL("MSVDXQUE: Need immediate dequeue\n");
			psb_msvdx_dequeue_send(dev);
		}
	}

	return ret;
}

int psb_cmdbuf_video(struct drm_file *priv,
		     struct list_head *validate_list,
		     uint32_t fence_type,
		     struct drm_psb_cmdbuf_arg *arg,
		     struct ttm_buffer_object *cmd_buffer,
		     struct psb_ttm_fence_rep *fence_arg)
{
	struct drm_device *dev = priv->minor->dev;
	struct ttm_fence_object *fence;
	int ret;

	/*
	 * Check this. Doesn't seem right. Have fencing done AFTER command
	 * submission and make sure drm_psb_idle idles the MSVDX completely.
	 */
	ret =
		psb_submit_video_cmdbuf(dev, cmd_buffer, arg->cmdbuf_offset,
					arg->cmdbuf_size, NULL);
	if (ret)
		return ret;


	/* DRM_ERROR("Intel: Fix video fencing!!\n"); */
	psb_fence_or_sync(priv, PSB_ENGINE_VIDEO, fence_type,
			  arg->fence_flags, validate_list, fence_arg,
			  &fence);

	ttm_fence_object_unref(&fence);
	spin_lock(&cmd_buffer->bdev->fence_lock);
	if (cmd_buffer->sync_obj != NULL)
		ttm_fence_sync_obj_unref(&cmd_buffer->sync_obj);
	spin_unlock(&cmd_buffer->bdev->fence_lock);

	return 0;
}


static int psb_msvdx_send(struct drm_device *dev, void *cmd,
			  unsigned long cmd_size)
{
	int ret = 0;
	struct drm_psb_private *dev_priv = dev->dev_private;

	while (cmd_size > 0) {
		uint32_t cur_cmd_size = MEMIO_READ_FIELD(cmd, FWRK_GENMSG_SIZE);
		uint32_t cur_cmd_id = MEMIO_READ_FIELD(cmd, FWRK_GENMSG_ID);
		if (cur_cmd_size > cmd_size) {
			ret = -EINVAL;
			DRM_ERROR("MSVDX:cmd_size %lu cur_cmd_size %lu\n",
				  cmd_size, (unsigned long)cur_cmd_size);
			goto out;
		}

		/* Send the message to h/w */
		ret = psb_mtx_send(dev_priv, cmd);
		if (ret) {
			PSB_DEBUG_GENERAL("MSVDX: ret:%d\n", ret);
			goto out;
		}
		cmd += cur_cmd_size;
		cmd_size -= cur_cmd_size;
#if 0
		if (cur_cmd_id == VA_MSGID_DEBLOCK && IS_MRST(dev)) {
			cmd += sizeof(struct DEBLOCKPARAMS);
			cmd_size -= sizeof(struct DEBLOCKPARAMS);
		}
		if (cur_cmd_id == VA_MSGID_HOST_BE_OPP && IS_MRST(dev)) {
			cmd += sizeof(struct HOST_BE_OPP_PARAMS);
			cmd_size -= sizeof(struct HOST_BE_OPP_PARAMS);
		}
#endif

		if (cur_cmd_id == VA_MSGID_HOST_BE_OPP ||
		    cur_cmd_id == VA_MSGID_DEBLOCK ||
		    cur_cmd_id == VA_MSGID_OOLD) {
			cmd += (sizeof(FW_VA_DEBLOCK_MSG) - cur_cmd_size);
			cmd_size -= (sizeof(FW_VA_DEBLOCK_MSG) - cur_cmd_size);
		}
	}

out:
	PSB_DEBUG_GENERAL("MSVDX: ret:%d\n", ret);
	return ret;
}

int psb_mtx_send(struct drm_psb_private *dev_priv, const void *msg)
{
	static uint32_t pad_msg[FWRK_PADMSG_SIZE];
	const uint32_t *p_msg = (uint32_t *) msg;
	uint32_t msg_num, words_free, ridx, widx, buf_size, buf_offset;
	int ret = 0;

	PSB_DEBUG_GENERAL("MSVDX: psb_mtx_send\n");

	/* we need clocks enabled before we touch VEC local ram */
	PSB_WMSVDX32(clk_enable_all, MSVDX_MAN_CLK_ENABLE);

	msg_num = (MEMIO_READ_FIELD(msg, FWRK_GENMSG_SIZE) + 3) / 4;

#if 0
		{
			int i;
			printk(KERN_DEBUG "MSVDX: psb_mtx_send is %dDW\n",
			       msg_num);

			for(i = 0; i < msg_num; i++)
				printk(KERN_DEBUG "0x%08x ", p_msg[i]);
			printk(KERN_DEBUG "\n");
		}
#endif
	buf_size = PSB_RMSVDX32(MSVDX_COMMS_TO_MTX_BUF_SIZE) & ((1 << 16) - 1);

	if (msg_num > buf_size) {
		ret = -EINVAL;
		DRM_ERROR("MSVDX: message exceed maximum,ret:%d\n", ret);
		goto out;
	}

	ridx = PSB_RMSVDX32(MSVDX_COMMS_TO_MTX_RD_INDEX);
	widx = PSB_RMSVDX32(MSVDX_COMMS_TO_MTX_WRT_INDEX);


	buf_size = PSB_RMSVDX32(MSVDX_COMMS_TO_MTX_BUF_SIZE) & ((1 << 16) - 1);
	/*0x2000 is VEC Local Ram offset*/
	buf_offset =
		(PSB_RMSVDX32(MSVDX_COMMS_TO_MTX_BUF_SIZE) >> 16) + 0x2000;

	/* message would wrap, need to send a pad message */
	if (widx + msg_num > buf_size) {
		/* Shouldn't happen for a PAD message itself */
		BUG_ON(MEMIO_READ_FIELD(msg, FWRK_GENMSG_ID)
		       == FWRK_MSGID_PADDING);

		/* if the read pointer is at zero then we must wait for it to
		 * change otherwise the write pointer will equal the read
		 * pointer,which should only happen when the buffer is empty
		 *
		 * This will only happens if we try to overfill the queue,
		 * queue management should make
		 * sure this never happens in the first place.
		 */
		BUG_ON(0 == ridx);
		if (0 == ridx) {
			ret = -EINVAL;
			DRM_ERROR("MSVDX: RIndex=0, ret:%d\n", ret);
			goto out;
		}

		/* Send a pad message */
		MEMIO_WRITE_FIELD(pad_msg, FWRK_GENMSG_SIZE,
				  (buf_size - widx) << 2);
		MEMIO_WRITE_FIELD(pad_msg, FWRK_GENMSG_ID,
				  FWRK_MSGID_PADDING);
		psb_mtx_send(dev_priv, pad_msg);
		widx = PSB_RMSVDX32(MSVDX_COMMS_TO_MTX_WRT_INDEX);
	}

	if (widx >= ridx)
		words_free = buf_size - (widx - ridx) - 1;
	else
		words_free = ridx - widx - 1;

	BUG_ON(msg_num > words_free);
	if (msg_num > words_free) {
		ret = -EINVAL;
		DRM_ERROR("MSVDX: msg_num > words_free, ret:%d\n", ret);
		goto out;
	}
	while (msg_num > 0) {
		PSB_WMSVDX32(*p_msg++, buf_offset + (widx << 2));
		msg_num--;
		widx++;
		if (buf_size == widx)
			widx = 0;
	}

	PSB_WMSVDX32(widx, MSVDX_COMMS_TO_MTX_WRT_INDEX);

	/* Make sure clocks are enabled before we kick */
	PSB_WMSVDX32(clk_enable_all, MSVDX_MAN_CLK_ENABLE);

	/* signal an interrupt to let the mtx know there is a new message */
	/* PSB_WMSVDX32(1, MSVDX_MTX_KICKI); */
	PSB_WMSVDX32(1, MSVDX_MTX_KICK);

	/* Read MSVDX Register several times in case Idle signal assert */
	PSB_RMSVDX32(MSVDX_INTERRUPT_STATUS);
	PSB_RMSVDX32(MSVDX_INTERRUPT_STATUS);
	PSB_RMSVDX32(MSVDX_INTERRUPT_STATUS);
	PSB_RMSVDX32(MSVDX_INTERRUPT_STATUS);


out:
	return ret;
}

static int psb_msvdx_towpass_deblock(struct drm_device *dev,
				     uint32_t *pPicparams)
{
	struct drm_psb_private *dev_priv =
		(struct drm_psb_private *)dev->dev_private;
	uint32_t cmd_size, cmd_count = 0;
	uint32_t cmd_id, reg, value, wait, reg_value, read = 0, ret = 0;

	cmd_size = *pPicparams++;
	PSB_DEBUG_GENERAL("MSVDX DEBLOCK: deblock get cmd size %d\n", cmd_size);
	/* printk("MSVDX DEBLOCK: deblock get cmd size %d\n", cmd_size); */

	do {
		cmd_id = (*pPicparams) & 0xf0000000;
		reg = (*pPicparams++)  & 0x0fffffff;
		switch (cmd_id) {
		case MSVDX_DEBLOCK_REG_SET: {
			value = *pPicparams++;
			PSB_WMSVDX32(value, reg);
			cmd_count += 2;
			break;
		}
		case MSVDX_DEBLOCK_REG_GET: {
			read = PSB_RMSVDX32(reg);
			cmd_count += 1;
			break;
		}
		case MSVDX_DEBLOCK_REG_POLLn: {
			value = *pPicparams++;
			wait = 0;

			do {
				reg_value = PSB_RMSVDX32(reg);
			} while ((wait++ < 20000) && (value > reg_value));

			if (wait >= 20000) {
				ret = 1;
				PSB_DEBUG_GENERAL(
					"MSVDX DEBLOCK: polln cmd space time out!\n");
				goto finish_deblock;
			}
			cmd_count += 2;
			break;
		}
		case MSVDX_DEBLOCK_REG_POLLx: {
			wait = 0;

			do {
				reg_value = PSB_RMSVDX32(reg);
			} while ((wait++ < 20000) && (read > reg_value));

			if (wait >= 20000) {
				ret = 1;
				PSB_DEBUG_GENERAL(
					"MSVDX DEBLOCK: pollx cmd space time out!\n");
				goto finish_deblock;
			}

			cmd_count += 1;
			break;
		}
		default:
			ret = 1;
			PSB_DEBUG_GENERAL(
				"MSVDX DEBLOCK: get error cmd_id: 0x%x!\n",
				cmd_id);
			PSB_DEBUG_GENERAL(
				"MSVDX DEBLOCK: execute cmd num is %d\n",
				cmd_count);
			/* printk("MSVDX DEBLOCK: get error cmd_id: 0x%x!\n",
			   cmd_id); */
			/* printk("MSVDX DEBLOCK: execute cmd num is %d\n",
			   cmd_count); */
			goto finish_deblock;
		}
	} while (cmd_count < cmd_size);


finish_deblock:
	PSB_DEBUG_GENERAL("MSVDX DEBLOCK: execute cmd num is %d\n", cmd_count);
	return ret;
}

/*
 * MSVDX MTX interrupt
 */
static void psb_msvdx_mtx_interrupt(struct drm_device *dev)
{
	struct drm_psb_private *dev_priv =
		(struct drm_psb_private *)dev->dev_private;
	static uint32_t buf[128]; /* message buffer */
	uint32_t ridx, widx, buf_size, buf_offset;
	uint32_t num, ofs; /* message num and offset */
	struct msvdx_private *msvdx_priv = dev_priv->msvdx_private;
	int i;

	PSB_DEBUG_GENERAL("MSVDX:Got a MSVDX MTX interrupt\n");

	/* Are clocks enabled  - If not enable before
	 * attempting to read from VLR
	 */
	if (PSB_RMSVDX32(MSVDX_MAN_CLK_ENABLE) != (clk_enable_all)) {
		PSB_DEBUG_GENERAL("MSVDX:Clocks disabled when Interupt set\n");
		PSB_WMSVDX32(clk_enable_all, MSVDX_MAN_CLK_ENABLE);
	}


loop: /* just for coding style check */
	ridx = PSB_RMSVDX32(MSVDX_COMMS_TO_HOST_RD_INDEX);
	widx = PSB_RMSVDX32(MSVDX_COMMS_TO_HOST_WRT_INDEX);

	/* Get out of here if nothing */
	if (ridx == widx)
		goto done;

	buf_size = PSB_RMSVDX32(MSVDX_COMMS_TO_HOST_BUF_SIZE) & ((1 << 16) - 1);
	/*0x2000 is VEC Local Ram offset*/
	buf_offset =
		(PSB_RMSVDX32(MSVDX_COMMS_TO_HOST_BUF_SIZE) >> 16) + 0x2000;

	ofs = 0;
	buf[ofs] = PSB_RMSVDX32(buf_offset + (ridx << 2));

	/* round to nearest word */
	num = (MEMIO_READ_FIELD(buf, FWRK_GENMSG_SIZE) + 3) / 4;

	/* ASSERT(num <= sizeof(buf) / sizeof(uint32_t)); */

	if (++ridx >= buf_size)
		ridx = 0;

	for (ofs++; ofs < num; ofs++) {
		buf[ofs] = PSB_RMSVDX32(buf_offset + (ridx << 2));

		if (++ridx >= buf_size)
			ridx = 0;
	}

	/* Update the Read index */
	PSB_WMSVDX32(ridx, MSVDX_COMMS_TO_HOST_RD_INDEX);

	if (msvdx_priv->msvdx_needs_reset)
		goto loop;

	switch (MEMIO_READ_FIELD(buf, FWRK_GENMSG_ID)) {
	case VA_MSGID_CMD_HW_PANIC:
	case VA_MSGID_CMD_FAILED: {
		/* For VXD385 firmware, fence value is not validate here */
		uint32_t msg_id = MEMIO_READ_FIELD(buf, FWRK_GENMSG_ID);
		uint32_t diff = 0;
		uint32_t fence, fault;
		uint32_t last_mb, first_mb;
		drm_psb_msvdx_frame_info_t *failed_frame = NULL;

		if (msg_id == VA_MSGID_CMD_HW_PANIC)
			PSB_DEBUG_MSVDX("MSVDX_DEBUG: get panic message.\n");
		else
			PSB_DEBUG_MSVDX("MSVDX_DEBUG: get failed message.\n");

		if (msg_id == VA_MSGID_CMD_HW_PANIC) {
			fence = MEMIO_READ_FIELD(buf,
						 FW_VA_HW_PANIC_FENCE_VALUE);
			first_mb = MEMIO_READ_FIELD(buf,
						    FW_VA_HW_PANIC_FIRST_MB_NUM);
			last_mb = MEMIO_READ_FIELD(buf,
						   FW_VA_HW_PANIC_FAULT_MB_NUM);
			PSB_DEBUG_MSVDX("MSVDX: PANIC fence is 0x%x\n",
					MEMIO_READ_FIELD(buf,
						FW_VA_HW_PANIC_FENCE_VALUE));
			PSB_DEBUG_MSVDX("MSVDX: PANIC first mb num is %d\n",
					MEMIO_READ_FIELD(buf,
						FW_VA_HW_PANIC_FIRST_MB_NUM));
			PSB_DEBUG_MSVDX("MSVDX: PANIC fault mb num is %d\n",
					MEMIO_READ_FIELD(buf,
						FW_VA_HW_PANIC_FAULT_MB_NUM));
			PSB_DEBUG_MSVDX("MSVDX: PANIC fe status is 0x%x\n",
					MEMIO_READ_FIELD(buf,
						FW_VA_HW_PANIC_FESTATUS));
			PSB_DEBUG_MSVDX("MSVDX: PANIC be status is 0x%x\n",
					MEMIO_READ_FIELD(buf,
						FW_VA_HW_PANIC_BESTATUS));
		} else {
			fence = MEMIO_READ_FIELD(buf,
						 FW_VA_CMD_FAILED_FENCE_VALUE);
			PSB_DEBUG_MSVDX("MSVDX_DEBUG: FAILED MESSAGE fence is %d.\n", MEMIO_READ_FIELD(buf, FW_VA_HW_PANIC_FIRST_MB_NUM));
			PSB_DEBUG_MSVDX("MSVDX_DEBUG: FAILED MESSAGE flag is %d.\n", MEMIO_READ_FIELD(buf, FW_VA_CMD_FAILED_FLAGS));
		}
		if (IS_MDFLD(dev) && IS_FW_UPDATED) 
		{
			fence = MEMIO_READ_FIELD(buf, FW_DEVA_CMD_FAILED_MSG_ID);
			fault = MEMIO_READ_FIELD(buf, FW_DEVA_CMD_FAILED_FLAGS);
		}

		if (msg_id == VA_MSGID_CMD_HW_PANIC)
			PSB_DEBUG_GENERAL("MSVDX: VA_MSGID_CMD_HW_PANIC:"
					  "Fault detected"
					  " - Fence: %08x"
					  " - resetting and ignoring error\n",
					  fence);
		else
			PSB_DEBUG_GENERAL("MSVDX: VA_MSGID_CMD_FAILED:"
					  "Fault detected"
					  " - Fence: %08x"
					  " - resetting and ignoring error\n",
					  fence);

		if (IS_D0(dev))
			msvdx_priv->msvdx_needs_reset |= MSVDX_RESET_NEEDS_REUPLOAD_FW |
				MSVDX_RESET_NEEDS_INIT_FW;
		else
			msvdx_priv->msvdx_needs_reset = 1;

		if (msg_id == VA_MSGID_CMD_HW_PANIC) {
			diff = msvdx_priv->msvdx_current_sequence
			       - dev_priv->sequence[PSB_ENGINE_VIDEO];

			if (diff > 0x0FFFFFFF)
				msvdx_priv->msvdx_current_sequence++;

			PSB_DEBUG_GENERAL("MSVDX: Fence ID missing, "
					  "assuming %08x\n",
					  msvdx_priv->msvdx_current_sequence);
		} else {
			msvdx_priv->msvdx_current_sequence = fence;
		}

		psb_fence_error(dev, PSB_ENGINE_VIDEO,
				msvdx_priv->msvdx_current_sequence,
				_PSB_FENCE_TYPE_EXE, DRM_CMD_FAILED);

		/* Flush the command queue */
		psb_msvdx_flush_cmd_queue(dev);

		if (msvdx_priv->host_be_opp_enabled) {
			/*get the frame_info struct for error concealment frame*/
			for (i = 0; i < MAX_DECODE_BUFFERS; i++) {
				/*by default the fence is 0, so there is problem here???*/
				if (msvdx_priv->frame_info[i].fence == fence) {
					failed_frame = &msvdx_priv->frame_info[i];
					break;
				}
			}
			if (!failed_frame) {
				DRM_ERROR("MSVDX: didn't find frame_info which matched the fence %d in failed/panic message\n", fence);
				goto done;
			}

			failed_frame->fw_status = 1; /* set ERROR flag */
		} else
			msvdx_priv->fw_status = 1; /* set ERROR flag */

		goto done;
	}
	case VA_MSGID_CMD_COMPLETED: {
		uint32_t fence;
		uint32_t flags = MEMIO_READ_FIELD(buf, FW_VA_CMD_COMPLETED_FLAGS);
		/* uint32_t last_mb = MEMIO_READ_FIELD(buf,
		   FW_VA_CMD_COMPLETED_LASTMB);
		*/
		if (IS_MDFLD(dev) && IS_FW_UPDATED)
			fence = MEMIO_READ_FIELD(buf, FW_VA_CMD_COMPLETED_MSG_ID);
		else
			fence = MEMIO_READ_FIELD(buf, FW_VA_CMD_COMPLETED_FENCE_VALUE);

		PSB_DEBUG_GENERAL("MSVDX:VA_MSGID_CMD_COMPLETED: "
				  "FenceID: %08x, flags: 0x%x\n",
				  fence, flags);

		msvdx_priv->msvdx_current_sequence = fence;
		msvdx_priv->ref_pic_fence = fence;

		psb_fence_handler(dev, PSB_ENGINE_VIDEO);

		if (flags & FW_VA_RENDER_HOST_INT) {
			/*Now send the next command from the msvdx cmd queue */
			psb_msvdx_dequeue_send(dev);
			goto done;
		}

		break;
	}
	case VA_MSGID_CMD_COMPLETED_BATCH: {
		uint32_t fence = MEMIO_READ_FIELD(buf,
						  FW_VA_CMD_COMPLETED_FENCE_VALUE);
		uint32_t tickcnt = MEMIO_READ_FIELD(buf,
						    FW_VA_CMD_COMPLETED_NO_TICKS);
		(void)tickcnt;
		/* we have the fence value in the message */
		PSB_DEBUG_GENERAL("MSVDX:VA_MSGID_CMD_COMPLETED_BATCH:"
				  " FenceID: %08x, TickCount: %08x\n",
				  fence, tickcnt);
		msvdx_priv->msvdx_current_sequence = fence;

		break;
	}
	case VA_MSGID_ACK:
		PSB_DEBUG_GENERAL("MSVDX: VA_MSGID_ACK\n");
		break;

	case VA_MSGID_TEST1:
		PSB_DEBUG_GENERAL("MSVDX: VA_MSGID_TEST1\n");
		break;

		/* Penwell deblock is not implemented here */
	case VA_MSGID_DEBLOCK_REQUIRED:	{
		uint32_t ctxid = MEMIO_READ_FIELD(buf,
						  FW_VA_DEBLOCK_REQUIRED_CONTEXT);
		struct psb_msvdx_deblock_queue *msvdx_deblock;
		uint32_t fence = MEMIO_READ_FIELD(buf,
						  FW_VA_DEBLOCK_REQUIRED_FENCE_VALUE);

		PSB_DEBUG_GENERAL("MSVDX: VA_MSGID_DEBLOCK_REQUIRED"
				  " Fence=%08x\n", fence);


		/*deblock and on-be-opp use the same message, there is difficulty to distinguish them*/
		if (IS_MRST(dev) && 
		(msvdx_priv->deblock_enabled == 1)) {
			PSB_DEBUG_MSVDX("MSVDX_DEBUG: get deblock required message for deblock operation.\n");
			if (list_empty(&msvdx_priv->deblock_queue)) {
				PSB_DEBUG_GENERAL(
					"DEBLOCKQUE: deblock param list is empty\n");
				PSB_WMSVDX32(0, MSVDX_CMDS_END_SLICE_PICTURE);
				PSB_WMSVDX32(1, MSVDX_CMDS_END_SLICE_PICTURE);
				goto done;
			}
			msvdx_deblock = list_first_entry(&msvdx_priv->deblock_queue,
							 struct psb_msvdx_deblock_queue, head);

			if (0) {
				PSB_DEBUG_GENERAL("MSVDX DEBLOCK: by pass \n");
				/* try to unblock rendec */
				PSB_WMSVDX32(0, MSVDX_CMDS_END_SLICE_PICTURE);
				PSB_WMSVDX32(1, MSVDX_CMDS_END_SLICE_PICTURE);
				kfree(msvdx_deblock->dbParams.pPicparams);
				list_del(&msvdx_deblock->head);
				goto done;
			}


			if (ctxid != msvdx_deblock->dbParams.ctxid) {
				PSB_DEBUG_GENERAL("MSVDX DEBLOCK: wrong ctxid, may "
						  "caused by multiple context since "
						  "it's not supported yet\n");
				/* try to unblock rendec */
				PSB_WMSVDX32(0, MSVDX_CMDS_END_SLICE_PICTURE);
				PSB_WMSVDX32(1, MSVDX_CMDS_END_SLICE_PICTURE);
				kfree(msvdx_deblock->dbParams.pPicparams);
				list_del(&msvdx_deblock->head);
				goto done;
			}

			if (msvdx_deblock->dbParams.pPicparams) {
				PSB_DEBUG_GENERAL("MSVDX DEBLOCK: start deblocking\n");
				/* printk("MSVDX DEBLOCK: start deblocking\n"); */

				if (psb_msvdx_towpass_deblock(dev,
							      msvdx_deblock->dbParams.pPicparams)) {

					PSB_DEBUG_GENERAL(
						"MSVDX DEBLOCK: deblock fail!\n");
					PSB_WMSVDX32(0, MSVDX_CMDS_END_SLICE_PICTURE);
					PSB_WMSVDX32(1, MSVDX_CMDS_END_SLICE_PICTURE);
				}
				kfree(msvdx_deblock->dbParams.pPicparams);
			} else {
				PSB_DEBUG_GENERAL("MSVDX DEBLOCK: deblock abort!\n");
				/* printk("MSVDX DEBLOCK: deblock abort!\n"); */
				PSB_WMSVDX32(0, MSVDX_CMDS_END_SLICE_PICTURE);
				PSB_WMSVDX32(1, MSVDX_CMDS_END_SLICE_PICTURE);
			}

			list_del(&msvdx_deblock->head);
			kfree(msvdx_deblock);
		}
		else {
			struct psb_msvdx_ec_ctx *msvdx_ec_ctx = NULL;
			int found = 0;
			PSB_DEBUG_MSVDX("Get deblock required msg for ec\n");
			for (i = 0; i < PSB_MAX_EC_INSTANCE; i++)
				if (msvdx_priv->msvdx_ec_ctx[i]->fence
							== (fence & (~0xf))) {
					msvdx_ec_ctx =
						msvdx_priv->msvdx_ec_ctx[i];
					found++;
				}
			/* if found > 1, fence wrapping happens */
			if (!msvdx_ec_ctx ||
			    !(msvdx_ec_ctx->tfile) || found > 1) {
				PSB_DEBUG_MSVDX(
			"no matched ctx: fence 0x%x, found %d, ctx 0x%08x\n",
					fence, found, msvdx_ec_ctx);
				PSB_WMSVDX32(0, MSVDX_CMDS_END_SLICE_PICTURE);
				PSB_WMSVDX32(1, MSVDX_CMDS_END_SLICE_PICTURE);
				goto done;
			}

			msvdx_ec_ctx->cur_frame_info->fw_status = 1;

			/* try to unblock rendec */
			/*
			PSB_WMSVDX32(0, MSVDX_CMDS_END_SLICE_PICTURE);
			PSB_WMSVDX32(1, MSVDX_CMDS_END_SLICE_PICTURE);
			*/
			/*do error concealment with hw*/
			msvdx_priv->cur_msvdx_ec_ctx = msvdx_ec_ctx;
			schedule_work(&msvdx_priv->ec_work);
		}
		break;
	}

	case VA_MSGID_CMD_CONTIGUITY_WARNING: {
		drm_psb_msvdx_frame_info_t *ec_frame = NULL;
		drm_psb_msvdx_decode_status_t *fault_region = NULL;
		struct psb_msvdx_ec_ctx *msvdx_ec_ctx = NULL;
		uint32_t reg_idx;
		int found = 0;

		/*get erro info*/
		uint32_t fence = MEMIO_READ_FIELD(buf,
					FW_VA_CONTIGUITY_WARNING_FENCE_VALUE);
		uint32_t start = MEMIO_READ_FIELD(buf,
					FW_VA_CONTIGUITY_WARNING_BEGIN_MB_NUM);
		uint32_t end = MEMIO_READ_FIELD(buf,
					FW_VA_CONTIGUITY_WARNING_END_MB_NUM);
		PSB_DEBUG_MSVDX("MSVDX: get contiguity warning message\n");
		PSB_DEBUG_MSVDX("MSVDX: CONTIGUITY_WARNING fence %x\n",
				MEMIO_READ_FIELD(buf,
					FW_VA_CONTIGUITY_WARNING_FENCE_VALUE));
		PSB_DEBUG_MSVDX("MSVDX: CONTIGUITY_WARNING end mb num %d\n",
				MEMIO_READ_FIELD(buf,
					FW_VA_CONTIGUITY_WARNING_END_MB_NUM));
		PSB_DEBUG_MSVDX("MSVDX: CONTIGUITY_WARNING begin mb num %d\n",
			MEMIO_READ_FIELD(buf,
				FW_VA_CONTIGUITY_WARNING_BEGIN_MB_NUM));

		/*get the frame_info struct for error concealment frame*/
		for (i = 0; i < PSB_MAX_EC_INSTANCE; i++)
			if (msvdx_priv->msvdx_ec_ctx[i]->fence ==
							(fence & (~0xf))) {
				msvdx_ec_ctx = msvdx_priv->msvdx_ec_ctx[i];
				found++;
			}
		/* psb_msvdx_mtx_message_dump(dev); */
		if (!msvdx_ec_ctx || !(msvdx_ec_ctx->tfile) || found > 1) {
			PSB_DEBUG_MSVDX(
			"no matched ctx: fence 0x%x, found %d, ctx 0x%08x\n",
				fence, found, msvdx_ec_ctx);
			goto done;
		}

		/*
		ec_frame->fw_status = 1;
		fault_region = &ec_frame->decode_status;
		*/

		fault_region = &msvdx_ec_ctx->decode_status;
		if (start > end)
			start = end;
		if (start < PSB_MSVDX_EC_ROLLBACK)
			start = 0;
		else
			start -= PSB_MSVDX_EC_ROLLBACK;

		if (fault_region->num_region) {
			reg_idx = fault_region->num_region - 1;
			if ((start <= fault_region->mb_regions[reg_idx].end) &&
			    (end > fault_region->mb_regions[reg_idx].end))
				fault_region->mb_regions[reg_idx].end = end;
			else {
				reg_idx = fault_region->num_region++;
				fault_region->mb_regions[reg_idx].start = start;
				fault_region->mb_regions[reg_idx].end = end;
			}
		} else {
			reg_idx = fault_region->num_region++;
			fault_region->mb_regions[reg_idx].start = start;
			fault_region->mb_regions[reg_idx].end = end;
		}
		break;
	}
	default:
		DRM_ERROR("ERROR: msvdx Unknown message from MTX, ID:0x%08x\n", MEMIO_READ_FIELD(buf, FWRK_GENMSG_ID));
		goto done;
	}

done:
	PSB_DEBUG_GENERAL("MSVDX Interrupt: finish process a message\n");
	if (ridx != widx) {
		PSB_DEBUG_GENERAL("MSVDX Interrupt: there are more message to be read\n");
		goto loop;
	}

	/* we get a frame/slice done, try to save some power*/
	if (IS_D0(dev)) {
		if (drm_msvdx_pmpolicy == PSB_PMPOLICY_POWERDOWN)
			schedule_delayed_work(&dev_priv->scheduler.msvdx_suspend_wq, 0);
	} else {
		if (drm_msvdx_pmpolicy != PSB_PMPOLICY_NOPM)
			schedule_delayed_work(&dev_priv->scheduler.msvdx_suspend_wq, 0);
	}

	DRM_MEMORYBARRIER();	/* TBD check this... */
}


/*
 * MSVDX interrupt.
 */
IMG_BOOL psb_msvdx_interrupt(IMG_VOID *pvData)
{
	struct drm_device *dev;
	struct drm_psb_private *dev_priv;
	struct msvdx_private *msvdx_priv;
	uint32_t msvdx_stat;

	if (pvData == IMG_NULL) {
		DRM_ERROR("ERROR: msvdx %s, Invalid params\n", __func__);
		return IMG_FALSE;
	}

	dev = (struct drm_device *)pvData;
	if (!ospm_power_is_hw_on(OSPM_VIDEO_DEC_ISLAND) && IS_MRST(dev)) {
		DRM_ERROR("ERROR: interrupt arrived but HW is power off\n");
		return IMG_FALSE;
	}

	dev_priv = (struct drm_psb_private *) dev->dev_private;
	msvdx_priv = dev_priv->msvdx_private;

	msvdx_priv->msvdx_hw_busy = REG_READ(0x20D0) & (0x1 << 9);

	msvdx_stat = PSB_RMSVDX32(MSVDX_INTERRUPT_STATUS);

	/* driver only needs to handle mtx irq
	 * For MMU fault irq, there's always a HW PANIC generated
	 * if HW/FW is totally hang, the lockup function will handle
	 * the reseting
	 */
	if (!IS_D0(dev) &&
	    (msvdx_stat & MSVDX_INTERRUPT_STATUS_CR_MMU_FAULT_IRQ_MASK)) {
		/*Ideally we should we should never get to this */
		PSB_DEBUG_IRQ("MSVDX:MMU Fault:0x%x\n", msvdx_stat);

		/* Pause MMU */
		PSB_WMSVDX32(MSVDX_MMU_CONTROL0_CR_MMU_PAUSE_MASK,
			     MSVDX_MMU_CONTROL0);
		DRM_WRITEMEMORYBARRIER();

		/* Clear this interupt bit only */
		PSB_WMSVDX32(MSVDX_INTERRUPT_STATUS_CR_MMU_FAULT_IRQ_MASK,
			     MSVDX_INTERRUPT_CLEAR);
		PSB_RMSVDX32(MSVDX_INTERRUPT_CLEAR);
		DRM_READMEMORYBARRIER();

		msvdx_priv->msvdx_needs_reset = 1;
	} else if (msvdx_stat & MSVDX_INTERRUPT_STATUS_CR_MTX_IRQ_MASK) {
		PSB_DEBUG_IRQ
			("MSVDX: msvdx_stat: 0x%x(MTX)\n", msvdx_stat);

		/* Clear all interupt bits */
		if (IS_D0(dev))
			PSB_WMSVDX32(MSVDX_INTERRUPT_STATUS_CR_MTX_IRQ_MASK,
				     MSVDX_INTERRUPT_CLEAR);
		else
			PSB_WMSVDX32(0xffff, MSVDX_INTERRUPT_CLEAR);

		PSB_RMSVDX32(MSVDX_INTERRUPT_CLEAR);
		DRM_READMEMORYBARRIER();

		psb_msvdx_mtx_interrupt(dev);
	}

	return IMG_TRUE;
}


void psb_msvdx_lockup(struct drm_psb_private *dev_priv,
		      int *msvdx_lockup, int *msvdx_idle)
{
	int diff;
	struct msvdx_private *msvdx_priv = dev_priv->msvdx_private;

	*msvdx_lockup = 0;
	*msvdx_idle = 1;

#if 0
	PSB_DEBUG_GENERAL("MSVDXTimer: current_sequence:%d "
			  "last_sequence:%d and last_submitted_sequence :%d\n",
			  msvdx_priv->msvdx_current_sequence,
			  msvdx_priv->msvdx_last_sequence,
			  dev_priv->sequence[PSB_ENGINE_VIDEO]);
#endif

	diff = msvdx_priv->msvdx_current_sequence -
	       dev_priv->sequence[PSB_ENGINE_VIDEO];

	if (diff > 0x0FFFFFFF) {
		if (msvdx_priv->msvdx_current_sequence ==
		    msvdx_priv->msvdx_last_sequence) {
			DRM_ERROR("MSVDXTimer:locked-up for sequence:%d\n",
				  msvdx_priv->msvdx_current_sequence);
			*msvdx_lockup = 1;
		} else {
			PSB_DEBUG_GENERAL("MSVDXTimer: "
					  "msvdx responded fine so far\n");
			msvdx_priv->msvdx_last_sequence =
				msvdx_priv->msvdx_current_sequence;
			*msvdx_idle = 0;
		}
	}
}

int psb_check_msvdx_idle(struct drm_device *dev)
{
	struct drm_psb_private *dev_priv =
		(struct drm_psb_private *)dev->dev_private;
	struct msvdx_private *msvdx_priv = dev_priv->msvdx_private;
	/* drm_psb_msvdx_frame_info_t *current_frame = NULL; */

	if (msvdx_priv->msvdx_fw_loaded == 0)
		return 0;

	if (msvdx_priv->msvdx_busy) {
		PSB_DEBUG_PM("MSVDX: psb_check_msvdx_idle returns busy\n");
		return -EBUSY;
	}

	if (IS_D0(dev)) {
		PSB_DEBUG_MSVDX("   SIGNITURE is %x\n", PSB_RMSVDX32(MSVDX_COMMS_SIGNATURE));

		if (!(PSB_RMSVDX32(MSVDX_COMMS_FW_STATUS) & MSVDX_FW_STATUS_HW_IDLE))
			return -EBUSY;
	}
	/*
		if (msvdx_priv->msvdx_hw_busy) {
			PSB_DEBUG_PM("MSVDX: %s, HW is busy\n", __func__);
			return -EBUSY;
		}
	*/
	return 0;
}


int psb_remove_videoctx(struct drm_psb_private *dev_priv, struct file *filp)
{
	struct psb_video_ctx *pos, *n;
	/* iterate to query all ctx to if there is DRM running*/
	ied_enabled = 0;

	list_for_each_entry_safe(pos, n, &dev_priv->video_ctx, head) {
		if (pos->filp == filp) {
			PSB_DEBUG_GENERAL("Video:remove context profile %d,"
					  " entrypoint %d\n",
					  (pos->ctx_type >> 8) & 0xff,
					  (pos->ctx_type & 0xff));

			/* if current ctx points to it, set to NULL */
			if (dev_priv->topaz_ctx == pos) {
				/*Reset fw load status here.*/
				if (IS_MDFLD(dev_priv->dev) &&
					(VAEntrypointEncSlice ==
						(pos->ctx_type & 0xff)
					|| VAEntrypointEncPicture ==
						(pos->ctx_type & 0xff)))
					pnw_reset_fw_status(dev_priv->dev);

				dev_priv->topaz_ctx = NULL;
			} else if (IS_MDFLD(dev_priv->dev) &&
					(VAEntrypointEncSlice ==
						(pos->ctx_type & 0xff)
					|| VAEntrypointEncPicture ==
						(pos->ctx_type & 0xff)))
				PSB_DEBUG_GENERAL("Remove a inactive "\
						"encoding context.\n");

			if (dev_priv->last_topaz_ctx == pos)
				dev_priv->last_topaz_ctx = NULL;

			if (dev_priv->msvdx_ctx == pos)
				dev_priv->msvdx_ctx = NULL;
			if (dev_priv->last_msvdx_ctx == pos)
				dev_priv->last_msvdx_ctx = NULL;

			list_del(&pos->head);
			kfree(pos);
		}  else {
			if (pos->ctx_type & VA_RT_FORMAT_PROTECTED)
				ied_enabled = 1;
		}
	}
	return 0;
}
struct psb_video_ctx *psb_find_videoctx(struct drm_psb_private *dev_priv,
					struct file *filp)
{
	struct psb_video_ctx *pos, *n;

	list_for_each_entry_safe(pos, n, &dev_priv->video_ctx, head) {
		if (pos->filp == filp)
			return pos;
	}
	return NULL;
}

static int psb_entrypoint_number(struct drm_psb_private *dev_priv,
		uint32_t entry_type)
{
	struct psb_video_ctx *pos, *n;
	int count = 0;

	entry_type &= 0xff;

	if (entry_type < VAEntrypointVLD ||
			entry_type > VAEntrypointEncPicture) {
		DRM_ERROR("Invalide entrypoint value %d.\n", entry_type);
		return -EINVAL;
	}

	list_for_each_entry_safe(pos, n, &dev_priv->video_ctx, head) {
		if (IS_MDFLD(dev_priv->dev) &&
				(entry_type == (pos->ctx_type & 0xff)))
			count++;

	}

	PSB_DEBUG_GENERAL("There are %d active entrypoint %d.\n",
			count, entry_type);
	return count;
}


int lnc_video_getparam(struct drm_device *dev, void *data,
		       struct drm_file *file_priv)
{
	struct drm_lnc_video_getparam_arg *arg = data;
	int ret = 0;
	struct drm_psb_private *dev_priv =
		(struct drm_psb_private *)file_priv->minor->dev->dev_private;
	drm_psb_msvdx_frame_info_t *current_frame = NULL;
	struct ttm_object_file *tfile = psb_fpriv(file_priv)->tfile;
	uint32_t handle, i;
	uint32_t offset = 0;
	uint32_t device_info = 0;
	uint32_t ctx_type = 0;
	struct psb_video_ctx *video_ctx = NULL;
	uint32_t rar_ci_info[2];
	struct msvdx_private *msvdx_priv = dev_priv->msvdx_private;

	switch (arg->key) {
	case LNC_VIDEO_GETPARAM_RAR_INFO:
		rar_ci_info[0] = dev_priv->rar_region_start;
		rar_ci_info[1] = dev_priv->rar_region_size;
		ret = copy_to_user((void __user *)((unsigned long)arg->value),
				   &rar_ci_info[0],
				   sizeof(rar_ci_info));
		break;
	case LNC_VIDEO_GETPARAM_CI_INFO:
		rar_ci_info[0] = dev_priv->ci_region_start;
		rar_ci_info[1] = dev_priv->ci_region_size;
		ret = copy_to_user((void __user *)((unsigned long)arg->value),
				   &rar_ci_info[0],
				   sizeof(rar_ci_info));
		break;
	case LNC_VIDEO_FRAME_SKIP:
		/* Medfield should not call it */
		ret = -EFAULT;
		break;
	case LNC_VIDEO_DEVICE_INFO:
		device_info = 0xffff & dev_priv->video_device_fuse;
		device_info |= (0xffff & dev->pci_device) << 16;

		ret = copy_to_user((void __user *)((unsigned long)arg->value),
				   &device_info, sizeof(device_info));
		break;
	case IMG_VIDEO_NEW_CONTEXT:
		/* add video decode/encode context */
		ret = copy_from_user(&ctx_type, (void __user *)((unsigned long)arg->value),
				     sizeof(ctx_type));
		video_ctx = kmalloc(sizeof(struct psb_video_ctx), GFP_KERNEL);
		if (video_ctx == NULL) {
			ret = -ENOMEM;
			break;
		}
		INIT_LIST_HEAD(&video_ctx->head);
		video_ctx->ctx_type = ctx_type;
		video_ctx->filp = file_priv->filp;
		list_add(&video_ctx->head, &dev_priv->video_ctx);

		if (IS_MDFLD(dev_priv->dev) &&
				(VAEntrypointEncSlice ==
				 (ctx_type & 0xff)))
			pnw_reset_fw_status(dev_priv->dev);

		PSB_DEBUG_GENERAL("Video:add ctx profile %d, entry %d.\n",
					((ctx_type >> 8) & 0xff),
					(ctx_type & 0xff));
		PSB_DEBUG_GENERAL("Video:add context protected 0x%x.\n",
					(ctx_type & VA_RT_FORMAT_PROTECTED));
		if (ctx_type & VA_RT_FORMAT_PROTECTED)
			ied_enabled = 1;
		break;

	case IMG_VIDEO_RM_CONTEXT:
		psb_remove_videoctx(dev_priv, file_priv->filp);
		break;
#if 0 //TODO: add this with usr space update
	case IMG_VIDEO_UPDATE_CONTEXT:
		ret = copy_from_user(&ctx_type,
				(void __user *)((unsigned long)arg->value),
				sizeof(ctx_type));
		video_ctx = psb_find_videoctx(dev_priv, file_priv->filp);
		if (video_ctx) {
			PSB_DEBUG_GENERAL(
				"Video: update video ctx old value 0x%08x\n",
				video_ctx->ctx_type);
			video_ctx->ctx_type = ctx_type;
			PSB_DEBUG_GENERAL(
				"Video: update video ctx new value 0x%08x\n",
				video_ctx->ctx_type);
		} else
			PSB_DEBUG_GENERAL(
				"Video:fail to find context profile %d, entrypoint %d",
				(ctx_type >> 8), (ctx_type & 0xff));
#endif
		break;
	case IMG_VIDEO_DECODE_STATUS:
		if (msvdx_priv->host_be_opp_enabled) {
			/*get the right frame_info struct for current surface*/
			ret = copy_from_user(&handle,
					     (void __user *)((unsigned long)arg->arg), 4);
			if (ret) {
				DRM_ERROR("MSVDX in lnc_video_getparam, copy_from_user failed.\n");
				break;
			}

			for (i = 0; i < MAX_DECODE_BUFFERS; i++) {
				if (msvdx_priv->frame_info[i].handle == handle) {
					current_frame = &msvdx_priv->frame_info[i];
					break;
				}
			}
			if (!current_frame) {
				DRM_ERROR("MSVDX: didn't find frame_info which matched the surface_id. \n");
				return -EFAULT;
			}
			ret = copy_to_user((void __user *)((unsigned long)arg->value),
					   &current_frame->fw_status, sizeof(current_frame->fw_status));
		} else
			ret = copy_to_user((void __user *)((unsigned long)arg->value),
					   &msvdx_priv->fw_status, sizeof(msvdx_priv->fw_status));
		break;

	case IMG_VIDEO_MB_ERROR:
		/*get the right frame_info struct for current surface*/
		ret = copy_from_user(&handle,
				     (void __user *)((unsigned long)arg->arg), 4);
		if (ret)
			break;

		for (i = 0; i < MAX_DECODE_BUFFERS; i++) {
			if (msvdx_priv->frame_info[i].handle == handle) {
				current_frame = &msvdx_priv->frame_info[i];
				break;
			}
		}
		if (!current_frame) {
			DRM_ERROR("MSVDX: didn't find frame_info which matched the surface_id. \n");
			return -EFAULT;
		}
		ret = copy_to_user((void __user *)((unsigned long)arg->value),
				   &(current_frame->decode_status), sizeof(drm_psb_msvdx_decode_status_t));
		if (ret) {
			DRM_ERROR("lnc_video_getparam copy_to_user error.\n");
			return -EFAULT;
		}
		break;
	case IMG_VIDEO_SET_DISPLAYING_FRAME:
		ret = copy_from_user(&msvdx_priv->displaying_frame,
				(void __user *)((unsigned long)arg->value),
				sizeof(msvdx_priv->displaying_frame));
		if (ret) {
			DRM_ERROR("IMG_VIDEO_SET_DISPLAYING_FRAME error.\n");
			return -EFAULT;
		}
		break;
	case IMG_VIDEO_GET_DISPLAYING_FRAME:
		ret = copy_to_user((void __user *)((unsigned long)arg->value),
				&msvdx_priv->displaying_frame,
				sizeof(msvdx_priv->displaying_frame));
		if (ret) {
			DRM_ERROR("IMG_VIDEO_GET_DISPLAYING_FRAME error.\n");
			return -EFAULT;
		}
		break;
	case IMG_DISPLAY_SET_WIDI_EXT_STATE:
		DRM_ERROR("variable drm_psb_widi has been removed\n");
		break;
	case IMG_VIDEO_GET_HDMI_STATE:
#ifdef IMG_VIDEO_SUPPORT_HDMI_STATE_SETTING
		ret = copy_to_user((void __user *)((unsigned long)arg->value),
				&hdmi_state,
				sizeof(hdmi_state));
		if (ret) {
			DRM_ERROR("IMG_VIDEO_GET_HDMI_STATE error.\n");
			return -EFAULT;
		}
#endif /* IMG_VIDEO_SUPPORT_HDMI_STATE_SETTING */
		break;
	case IMG_VIDEO_SET_HDMI_STATE:
#ifdef IMG_VIDEO_SUPPORT_HDMI_STATE_SETTING
		if (!hdmi_state) {
			PSB_DEBUG_ENTRY(
				"wait 100ms for kernel hdmi pipe ready.\n");
			msleep(100);
		}
		if (dev_priv->bhdmiconnected)
			hdmi_state = (int)arg->value;
		else
#endif /* IMG_VIDEO_SUPPORT_HDMI_STATE_SETTING */
			PSB_DEBUG_ENTRY(
				"skip hdmi_state setting, for unplugged.\n");

#ifdef IMG_VIDEO_SUPPORT_HDMI_STATE_SETTING
		PSB_DEBUG_ENTRY("%s, set hdmi_state = %d\n",
				 __func__, hdmi_state);
#endif /* IMG_VIDEO_SUPPORT_HDMI_STATE_SETTING */
		break;
	case PNW_VIDEO_QUERY_ENTRY:
		ret = copy_from_user(&handle,
				(void __user *)((unsigned long)arg->arg),
				sizeof(handle));
		if (ret)
			break;
		/*Return the number of active entries*/
		i = psb_entrypoint_number(dev_priv, handle);
		if (i >= 0)
			ret = copy_to_user((void __user *)
					((unsigned long)arg->value),
					&i, sizeof(i));
		break;
		case IMG_VIDEO_IED_STATE:
		if (IS_MDFLD(dev)) {
			/* query IED status by register is not safe */
			/* need first power-on msvdx, while now only  */
			/* schedule vxd suspend wq in interrupt handler */
#if 0
			int ied_enable;
			/* VXD must be power on during query IED register */
			if (!ospm_power_using_hw_begin(OSPM_VIDEO_DEC_ISLAND,
					OSPM_UHB_FORCE_POWER_ON))
				return -EBUSY;
			/* wrong spec, IED should be located in pci device 2 */
			if (REG_READ(PSB_IED_DRM_CNTL_STATUS) & IED_DRM_VLD)
				ied_enable = 1;
			else
				ied_enable = 0;
			PSB_DEBUG_GENERAL("ied_enable is %d.\n", ied_enable);
			ospm_power_using_hw_end(OSPM_VIDEO_DEC_ISLAND);
#endif
			ret = copy_to_user((void __user *)
					((unsigned long)arg->value),
					&ied_enabled, sizeof(ied_enabled));
		} else { /* Moorestown should not call it */
			DRM_ERROR("IMG_VIDEO_IED_EANBLE error.\n");
			return -EFAULT;
		}
		break;
	default:
		ret = -EFAULT;
		break;
	}

	if (ret)
		return -EFAULT;

	return 0;
}

inline int psb_try_power_down_msvdx(struct drm_device *dev)
{
	ospm_apm_power_down_msvdx(dev);
	return 0;
}

int psb_msvdx_save_context(struct drm_device *dev)
{
	struct drm_psb_private *dev_priv =
		(struct drm_psb_private *)dev->dev_private;
	struct msvdx_private *msvdx_priv = dev_priv->msvdx_private;
	int offset = 0;

	if (IS_D0(dev))
		msvdx_priv->msvdx_needs_reset = MSVDX_RESET_NEEDS_INIT_FW;
	else
		msvdx_priv->msvdx_needs_reset = 1;

	for (offset = 0; offset < VEC_LOCAL_MEM_BYTE_SIZE / 4; ++offset)
		msvdx_priv->vec_local_mem_data[offset] =
			PSB_RMSVDX32(VEC_LOCAL_MEM_OFFSET + offset * 4);

	msvdx_priv->vec_local_mem_saved = 1;

	if (IS_D0(dev)) {
		PSB_WMSVDX32(0, MSVDX_MTX_ENABLE);
		psb_msvdx_reset(dev_priv);
		PSB_WMSVDX32(0, MSVDX_MAN_CLK_ENABLE);
	}

	return 0;
}

int psb_msvdx_restore_context(struct drm_device *dev)
{
	return 0;
}

int psb_msvdx_check_reset_fw(struct drm_device *dev)
{
	struct drm_psb_private *dev_priv = dev->dev_private;
	struct msvdx_private *msvdx_priv = dev_priv->msvdx_private;
	unsigned long irq_flags;

	spin_lock_irqsave(&msvdx_priv->msvdx_lock, irq_flags);
	/* reserve these code, once we use a unified firmware upload, we may
	 * have to support MRST with additional code change
	 */
	/* expected msvdx_needs_reset is set after previous session exited
	 * but msvdx_hw_busy is always 1, and caused powerdown not excuted
	 * so reload the firmware for every new context
	 */
	if (IS_MRST(dev) && (dev_priv->last_msvdx_ctx == NULL))
		msvdx_priv->msvdx_needs_reset |= MSVDX_RESET_NEEDS_REUPLOAD_FW |
			MSVDX_RESET_NEEDS_INIT_FW; /* re-load firmware */

	if (IS_MRST(dev) && dev_priv->msvdx_ctx && dev_priv->last_msvdx_ctx) {
		uint32_t from_profile, to_profile;

		from_profile = (dev_priv->last_msvdx_ctx->ctx_type >> 8) & 0xff;
		to_profile = (dev_priv->msvdx_ctx->ctx_type >> 8) & 0xff;

		/* not the same profile, and one of them is H264 constrained BP
		 * which needs Error Concealment firmware
		 */
		if ((from_profile != to_profile) &&
		    ((from_profile == VAProfileH264ConstrainedBaseline) ||
		     (to_profile == VAProfileH264ConstrainedBaseline))) {
			PSB_DEBUG_INIT("MSVDX: firmware switching needed\n");
			msvdx_priv->msvdx_needs_reset |= MSVDX_RESET_NEEDS_REUPLOAD_FW |
				MSVDX_RESET_NEEDS_INIT_FW; /* re-load firmware */
		}
	}
	/* handling fw upload here if required */
	/* power off first, then hw_begin will power up/upload FW correctly */
	if (msvdx_priv->msvdx_needs_reset & MSVDX_RESET_NEEDS_REUPLOAD_FW) {
		msvdx_priv->msvdx_needs_reset &= ~MSVDX_RESET_NEEDS_REUPLOAD_FW;
		ospm_power_island_down(OSPM_VIDEO_DEC_ISLAND);
	}
	spin_unlock_irqrestore(&msvdx_priv->msvdx_lock, irq_flags);
	return 0;
}
static void psb_msvdx_set_tile(struct drm_device *dev, unsigned long msvdx_tile)
{
	struct drm_psb_private *dev_priv = dev->dev_private;
	struct msvdx_private *msvdx_priv = dev_priv->msvdx_private;
	uint32_t cmd, msvdx_stride;
	uint32_t start = msvdx_priv->tile_region_start0;
	uint32_t end = msvdx_priv->tile_region_end0;

	msvdx_stride = (msvdx_tile & 0xf);
	/* Enable memory tiling */
	cmd = ((start >> 20) + (((end >> 20) - 1) << 12) +
				((0x8 | (msvdx_stride - 1)) << 24));
	if (msvdx_stride) {
		PSB_DEBUG_GENERAL("MSVDX: MMU Tiling register0 %08x\n", cmd);
		PSB_DEBUG_GENERAL("       Region 0x%08x-0x%08x\n",
					start, end);
		PSB_WMSVDX32(cmd, MSVDX_MMU_TILE_BASE0);
	}

	start = msvdx_priv->tile_region_start1;
	end = msvdx_priv->tile_region_end1;

	msvdx_stride = (msvdx_tile >> 4);
	/* Enable memory tiling */
	PSB_WMSVDX32(0, MSVDX_MMU_TILE_BASE1);
	cmd = ((start >> 20) + (((end >> 20) - 1) << 12) +
				((0x8 | (msvdx_stride - 1)) << 24));
	if (msvdx_stride) {
		PSB_DEBUG_GENERAL("MSVDX: MMU Tiling register1 %08x\n", cmd);
		PSB_DEBUG_GENERAL("       Region 0x%08x-0x%08x\n",
					start, end);
		PSB_WMSVDX32(cmd, MSVDX_MMU_TILE_BASE1);
	}
}
