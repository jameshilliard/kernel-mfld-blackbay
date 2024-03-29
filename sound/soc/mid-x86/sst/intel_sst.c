/*
 *  intel_sst.c - Intel SST Driver for audio engine
 *
 *  Copyright (C) 2008-10	Intel Corp
 *  Authors:	Vinod Koul <vinod.koul@intel.com>
 *		Harsha Priya <priya.harsha@intel.com>
 *		Dharageswari R <dharageswari.r@intel.com>
 *		KP Jeeja <jeeja.kp@intel.com>
 *  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 *  This driver exposes the audio engine functionalities to the ALSA
 *	 and middleware.
 *
 *  This file contains all init functions
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/pci.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/firmware.h>
#include <linux/miscdevice.h>
#include <linux/pm_runtime.h>
#include <linux/async.h>
#include <linux/lnw_gpio.h>
#include <linux/delay.h>
#include <asm/intel-mid.h>
#include <sound/intel_sst_ioctl.h>
#include "../sst_platform.h"
#include "intel_sst_fw_ipc.h"
#include "intel_sst_common.h"

MODULE_AUTHOR("Vinod Koul <vinod.koul@intel.com>");
MODULE_AUTHOR("Harsha Priya <priya.harsha@intel.com>");
MODULE_AUTHOR("Dharageswari R <dharageswari.r@intel.com>");
MODULE_AUTHOR("KP Jeeja <jeeja.kp@intel.com>");
MODULE_DESCRIPTION("Intel (R) SST(R) Audio Engine Driver");
MODULE_LICENSE("GPL v2");
MODULE_VERSION(SST_DRIVER_VERSION);

/* GPIO pins used for SSP3 */
#define CLV_I2S_3_CLK_GPIO_PIN	12
#define CLV_I2S_3_FS_GPIO_PIN	13
#define CLV_I2S_3_TXD_GPIO_PIN	74
#define CLV_I2S_3_RXD_GPIO_PIN	75
#define CLV_VIBRA_PWM_GPIO_PIN  49

struct intel_sst_drv *sst_drv_ctx;
static struct mutex drv_ctx_lock;
struct class *sst_class;

/* fops Routines */
static const struct file_operations intel_sst_fops = {
	.owner = THIS_MODULE,
	.open = intel_sst_open,
	.release = intel_sst_release,
	.read = intel_sst_read,
	.write = intel_sst_write,
	.unlocked_ioctl = intel_sst_ioctl,
	.mmap = intel_sst_mmap,
	.aio_read = intel_sst_aio_read,
	.aio_write = intel_sst_aio_write,
};
static const struct file_operations intel_sst_fops_cntrl = {
	.owner = THIS_MODULE,
	.open = intel_sst_open_cntrl,
	.release = intel_sst_release_cntrl,
	.unlocked_ioctl = intel_sst_ioctl,
};

static struct miscdevice lpe_dev = {
	.minor = MISC_DYNAMIC_MINOR,/* dynamic allocation */
	.name = "intel_sst",/* /dev/intel_sst */
	.fops = &intel_sst_fops
};


static struct miscdevice lpe_ctrl = {
	.minor = MISC_DYNAMIC_MINOR,/* dynamic allocation */
	.name = "intel_sst_ctrl",/* /dev/intel_sst_ctrl */
	.fops = &intel_sst_fops_cntrl
};

/**
* intel_sst_irq_thread - Interrupt service routine for SST
*
* @irq:	irq number of interrupt
* @context: pointer to device structre
*
* This function is called by OS when SST device raises
* an interrupt. This will be result of write in IPC register
* Source can be busy or done interrupt
*/
static irqreturn_t intel_sst_irq_thread(int irq, void *context)
{
	struct intel_sst_drv *drv = (struct intel_sst_drv *) context;
	union ipc_header header;
	struct stream_info *stream ;
	unsigned int size = 0, str_id;

	header.full = sst_shim_read(drv->shim, SST_IPCD);
	if (header.part.msg_id == IPC_SST_PERIOD_ELAPSED) {
		sst_clear_interrupt();
		str_id = header.part.str_id;
		stream = &sst_drv_ctx->streams[str_id];
		if (stream->period_elapsed)
			stream->period_elapsed(stream->pcm_substream);
		return IRQ_HANDLED;
	}
	pr_debug("%s:received IPC %x\n", __func__, header.full);
	if (header.part.large)
		size = header.part.data;
	if (header.part.msg_id & REPLY_MSG) {
		sst_drv_ctx->ipc_process_msg.header = header;
		memcpy_fromio(sst_drv_ctx->ipc_process_msg.mailbox,
			drv->mailbox + SST_MAILBOX_RCV, size);
		queue_work(sst_drv_ctx->process_msg_wq,
				&sst_drv_ctx->ipc_process_msg.wq);
	} else {
		sst_drv_ctx->ipc_process_reply.header = header;
		memcpy_fromio(sst_drv_ctx->ipc_process_reply.mailbox,
			drv->mailbox + SST_MAILBOX_RCV, size);
		queue_work(sst_drv_ctx->process_reply_wq,
				&sst_drv_ctx->ipc_process_reply.wq);
	}
	return IRQ_HANDLED;
}
/**
* intel_sst_interrupt - Interrupt service routine for SST
*
* @irq:	irq number of interrupt
* @context: pointer to device structre
*
* This function is called by OS when SST device raises
* an interrupt. This will be result of write in IPC register
* Source can be busy or done interrupt
*/
static irqreturn_t intel_sst_interrupt(int irq, void *context)
{
	union interrupt_reg isr, imr;
	union ipc_header header;
	struct intel_sst_drv *drv = (struct intel_sst_drv *) context;

	/* Do not handle interrupt in suspended state */
	if (drv->sst_state == SST_SUSPENDED)
		return IRQ_NONE;

	/* Interrupt arrived, check src */
	isr.full = sst_shim_read(drv->shim, SST_ISRX);
	if (isr.part.busy_interrupt) {
		/* mask busy interrupt */
		imr.full = sst_shim_read(drv->shim, SST_IMRX);
		imr.part.busy_interrupt = 1;
		sst_shim_write(sst_drv_ctx->shim, SST_IMRX, imr.full);
		return IRQ_WAKE_THREAD;
	} else if (isr.part.done_interrupt) {
		/* Clear done bit */
		header.full = sst_shim_read(drv->shim, SST_IPCX);
		header.part.done = 0;
		sst_shim_write(sst_drv_ctx->shim, SST_IPCX, header.full);
		/* write 1 to clear status register */;
		isr.part.done_interrupt = 1;
		sst_shim_write(sst_drv_ctx->shim, SST_ISRX, isr.full);
		queue_work(sst_drv_ctx->post_msg_wq,
			&sst_drv_ctx->ipc_post_msg.wq);
		return IRQ_HANDLED;
	} else
		return IRQ_NONE;
}


/*
* intel_sst_probe - PCI probe function
*
* @pci:	PCI device structure
* @pci_id: PCI device ID structure
*
* This function is called by OS when a device is found
* This enables the device, interrupt etc
*/
static int __devinit intel_sst_probe(struct pci_dev *pci,
			const struct pci_device_id *pci_id)
{
	int i, ret = 0;

	pr_debug("Probe for DID %x\n", pci->device);
	mutex_lock(&drv_ctx_lock);
	if (sst_drv_ctx) {
		pr_err("Only one sst handle is supported\n");
		mutex_unlock(&drv_ctx_lock);
		return -EBUSY;
	}

	sst_drv_ctx = kzalloc(sizeof(*sst_drv_ctx), GFP_KERNEL);
	if (!sst_drv_ctx) {
		pr_err("malloc fail\n");
		mutex_unlock(&drv_ctx_lock);
		return -ENOMEM;
	}
	mutex_unlock(&drv_ctx_lock);

	sst_drv_ctx->pci_id = pci->device;

	mutex_init(&sst_drv_ctx->stream_lock);
	mutex_init(&sst_drv_ctx->sst_lock);
	mutex_init(&sst_drv_ctx->mixer_ctrl_lock);

	sst_drv_ctx->stream_cnt = 0;
	sst_drv_ctx->encoded_cnt = 0;
	sst_drv_ctx->am_cnt = 0;
	sst_drv_ctx->pb_streams = 0;
	sst_drv_ctx->cp_streams = 0;
	sst_drv_ctx->unique_id = 0;
	sst_drv_ctx->pmic_port_instance = SST_DEFAULT_PMIC_PORT;
	sst_drv_ctx->fw = NULL;
	sst_drv_ctx->fw_in_mem = NULL;

	INIT_LIST_HEAD(&sst_drv_ctx->ipc_dispatch_list);
	INIT_WORK(&sst_drv_ctx->ipc_post_msg.wq, sst_post_message);
	INIT_WORK(&sst_drv_ctx->ipc_process_msg.wq, sst_process_message);
	INIT_WORK(&sst_drv_ctx->ipc_process_reply.wq, sst_process_reply);
	init_waitqueue_head(&sst_drv_ctx->wait_queue);

	sst_drv_ctx->mad_wq = create_singlethread_workqueue("sst_mad_wq");
	if (!sst_drv_ctx->mad_wq)
		goto do_free_drv_ctx;
	sst_drv_ctx->post_msg_wq = create_workqueue("sst_post_msg_wq");
	if (!sst_drv_ctx->post_msg_wq)
		goto free_mad_wq;
	sst_drv_ctx->process_msg_wq = create_workqueue("sst_process_msg_wqq");
	if (!sst_drv_ctx->process_msg_wq)
		goto free_post_msg_wq;
	sst_drv_ctx->process_reply_wq = create_workqueue("sst_proces_reply_wq");
	if (!sst_drv_ctx->process_reply_wq)
		goto free_process_msg_wq;

	for (i = 0; i < MAX_ACTIVE_STREAM; i++) {
		sst_drv_ctx->alloc_block[i].sst_id = BLOCK_UNINIT;
		sst_drv_ctx->alloc_block[i].ops_block.condition = false;
	}
	spin_lock_init(&sst_drv_ctx->list_spin_lock);

	sst_drv_ctx->max_streams = pci_id->driver_data;
	pr_debug("Got drv data max stream %d\n",
				sst_drv_ctx->max_streams);
	for (i = 1; i <= sst_drv_ctx->max_streams; i++) {
		struct stream_info *stream = &sst_drv_ctx->streams[i];
		INIT_LIST_HEAD(&stream->bufs);
		mutex_init(&stream->lock);
		spin_lock_init(&stream->pcm_lock);
	}
	if (sst_drv_ctx->pci_id == SST_MRST_PCI_ID) {
		sst_drv_ctx->mmap_mem = NULL;
		sst_drv_ctx->mmap_len = SST_MMAP_PAGES * PAGE_SIZE;
		while (sst_drv_ctx->mmap_len > 0) {
			sst_drv_ctx->mmap_mem =
				kzalloc(sst_drv_ctx->mmap_len, GFP_KERNEL);
			if (sst_drv_ctx->mmap_mem) {
				pr_debug("Got memory %p size 0x%x\n",
					sst_drv_ctx->mmap_mem,
					sst_drv_ctx->mmap_len);
				break;
			}
			if (sst_drv_ctx->mmap_len < (SST_MMAP_STEP*PAGE_SIZE)) {
				pr_err("mem alloc fail...abort!!\n");
				ret = -ENOMEM;
				goto free_process_reply_wq;
			}
			sst_drv_ctx->mmap_len -= (SST_MMAP_STEP * PAGE_SIZE);
			pr_debug("mem alloc failed...trying %d\n",
						sst_drv_ctx->mmap_len);
		}
	}
	if (sst_drv_ctx->pci_id == SST_CLV_PCI_ID) {
		sst_drv_ctx->device_input_mixer = SST_STREAM_DEVICE_IHF
							| SST_INPUT_STREAM_PCM;
	}

	/* Init the device */
	ret = pci_enable_device(pci);
	if (ret) {
		pr_err("device can't be enabled\n");
		goto do_free_mem;
	}
	sst_drv_ctx->pci = pci_dev_get(pci);
	ret = pci_request_regions(pci, SST_DRV_NAME);
	if (ret)
		goto do_disable_device;
	/* map registers */
	/* SST Shim */
	sst_drv_ctx->shim_phy_add = pci_resource_start(pci, 1);
	sst_drv_ctx->shim = pci_ioremap_bar(pci, 1);
	if (!sst_drv_ctx->shim)
		goto do_release_regions;
	pr_debug("SST Shim Ptr %p\n", sst_drv_ctx->shim);

	/* Shared SRAM */
	sst_drv_ctx->mailbox = pci_ioremap_bar(pci, 2);
	if (!sst_drv_ctx->mailbox)
		goto do_unmap_shim;
	pr_debug("SRAM Ptr %p\n", sst_drv_ctx->mailbox);

	/* IRAM */
	sst_drv_ctx->iram_base = pci_resource_start(pci, 3);
	sst_drv_ctx->iram = pci_ioremap_bar(pci, 3);
	if (!sst_drv_ctx->iram)
		goto do_unmap_sram;
	pr_debug("IRAM Ptr %p\n", sst_drv_ctx->iram);

	/* DRAM */
	sst_drv_ctx->dram_base = pci_resource_start(pci, 4);
	sst_drv_ctx->dram = pci_ioremap_bar(pci, 4);
	if (!sst_drv_ctx->dram)
		goto do_unmap_iram;
	pr_debug("DRAM Ptr %p\n", sst_drv_ctx->dram);

	sst_set_fw_state_locked(sst_drv_ctx, SST_UN_INIT);
	/* Register the ISR */
	ret = request_threaded_irq(pci->irq, intel_sst_interrupt,
			intel_sst_irq_thread, IRQF_SHARED, SST_DRV_NAME,
			sst_drv_ctx);
	if (ret)
		goto do_unmap_dram;
	pr_debug("Registered IRQ 0x%x\n", pci->irq);

	/*Register LPE Control as misc driver*/
	ret = misc_register(&lpe_ctrl);
	if (ret) {
		pr_err("couldn't register control device\n");
		goto do_free_irq;
	}

	if (sst_drv_ctx->pci_id == SST_MRST_PCI_ID) {
		ret = misc_register(&lpe_dev);
		if (ret) {
			pr_err("couldn't register LPE device\n");
			goto do_free_misc;
		}
	} else if ((sst_drv_ctx->pci_id == SST_MFLD_PCI_ID) ||
			(sst_drv_ctx->pci_id == SST_CLV_PCI_ID)) {
		u32 csr;
		u32 csr2;
		u32 clkctl;

		/*allocate mem for fw context save during suspend*/
		sst_drv_ctx->fw_cntx = kzalloc(FW_CONTEXT_MEM, GFP_KERNEL);
		if (!sst_drv_ctx->fw_cntx) {
			ret = -ENOMEM;
			goto do_free_misc;
		}
		/*setting zero as that is valid mem to restore*/
		sst_drv_ctx->fw_cntx_size = 0;

		/*set lpe start clock and ram size*/
		csr = sst_shim_read(sst_drv_ctx->shim, SST_CSR);
		csr |= 0x30000;
		/*make sure clksel set to OSC for SSP0,1 (default)*/
		csr &= 0xFFFFFFF3;
		sst_shim_write(sst_drv_ctx->shim, SST_CSR, csr);

		/*set clock output enable for SSP0,1,3*/
		clkctl = sst_shim_read(sst_drv_ctx->shim, SST_CLKCTL);
		if (sst_drv_ctx->pci_id == SST_CLV_PCI_ID)
			clkctl |= (0x7 << 16);
		else
			clkctl |= ((1<<16)|(1<<17));
		sst_shim_write(sst_drv_ctx->shim, SST_CLKCTL, clkctl);

		/* set SSP0 & SSP1 disable DMA Finish*/
		csr2 = sst_shim_read(sst_drv_ctx->shim, SST_CSR2);
		/*set SSP3 disable DMA finsh for SSSP3 */
		csr2 |= BIT(1)|BIT(2);
		sst_shim_write(sst_drv_ctx->shim, SST_CSR2, csr2);
	}

	/* GPIO_PIN 12,13,74,75 needs to be configured in
	 * ALT_FUNC_2 mode for SSP3 IOs
	 */
	if (sst_drv_ctx->pci_id == SST_CLV_PCI_ID) {
		lnw_gpio_set_alt(CLV_I2S_3_CLK_GPIO_PIN, LNW_ALT_2);
		lnw_gpio_set_alt(CLV_I2S_3_FS_GPIO_PIN, LNW_ALT_2);
		lnw_gpio_set_alt(CLV_I2S_3_TXD_GPIO_PIN, LNW_ALT_2);
		lnw_gpio_set_alt(CLV_I2S_3_RXD_GPIO_PIN, LNW_ALT_2);
		lnw_gpio_set_alt(CLV_VIBRA_PWM_GPIO_PIN, LNW_ALT_2);

		vibra_pwm_configure(true);
	}

	sst_drv_ctx->lpe_stalled = 0;
	pci_set_drvdata(pci, sst_drv_ctx);
	pm_runtime_allow(&pci->dev);
	pm_runtime_put_noidle(&pci->dev);
	register_sst(&pci->dev);
	pr_info("%s successfully done!\n", __func__);
	return ret;

do_free_misc:
	misc_deregister(&lpe_ctrl);
do_free_irq:
	free_irq(pci->irq, sst_drv_ctx);
do_unmap_dram:
	iounmap(sst_drv_ctx->dram);
do_unmap_iram:
	iounmap(sst_drv_ctx->iram);
do_unmap_sram:
	iounmap(sst_drv_ctx->mailbox);
do_unmap_shim:
	iounmap(sst_drv_ctx->shim);
do_release_regions:
	pci_release_regions(pci);
do_disable_device:
	pci_disable_device(pci);
do_free_mem:
	kfree(sst_drv_ctx->mmap_mem);
free_process_reply_wq:
	destroy_workqueue(sst_drv_ctx->process_reply_wq);
free_process_msg_wq:
	destroy_workqueue(sst_drv_ctx->process_msg_wq);
free_post_msg_wq:
	destroy_workqueue(sst_drv_ctx->post_msg_wq);
free_mad_wq:
	destroy_workqueue(sst_drv_ctx->mad_wq);
do_free_drv_ctx:
	kfree(sst_drv_ctx);
	sst_drv_ctx = NULL;
	pr_err("Probe failed with %d\n", ret);
	return ret;
}

/**
* intel_sst_remove - PCI remove function
*
* @pci:	PCI device structure
*
* This function is called by OS when a device is unloaded
* This frees the interrupt etc
*/
static void __devexit intel_sst_remove(struct pci_dev *pci)
{
	pm_runtime_get_noresume(&pci->dev);
	pm_runtime_forbid(&pci->dev);
	unregister_sst(&pci->dev);
	pci_dev_put(sst_drv_ctx->pci);
	sst_set_fw_state_locked(sst_drv_ctx, SST_UN_INIT);
	misc_deregister(&lpe_ctrl);
	free_irq(pci->irq, sst_drv_ctx);
	iounmap(sst_drv_ctx->dram);
	iounmap(sst_drv_ctx->iram);
	iounmap(sst_drv_ctx->mailbox);
	iounmap(sst_drv_ctx->shim);
	if (sst_drv_ctx->pci_id == SST_MRST_PCI_ID) {
		misc_deregister(&lpe_dev);
		kfree(sst_drv_ctx->mmap_mem);
	} else {
		kfree(sst_drv_ctx->fw_cntx);
	}
	kfree(sst_drv_ctx->runtime_param.param.addr);
	flush_scheduled_work();
	destroy_workqueue(sst_drv_ctx->process_reply_wq);
	destroy_workqueue(sst_drv_ctx->process_msg_wq);
	destroy_workqueue(sst_drv_ctx->post_msg_wq);
	destroy_workqueue(sst_drv_ctx->mad_wq);
	release_firmware(sst_drv_ctx->fw);
	sst_drv_ctx->fw = NULL;
	kfree(sst_drv_ctx->fw_sg_list.src);
	kfree(sst_drv_ctx->fw_sg_list.dst);
	sst_drv_ctx->fw_sg_list.list_len = 0;
	kfree(sst_drv_ctx->fw_in_mem);
	sst_drv_ctx->fw_in_mem = NULL;
	kfree(sst_drv_ctx);
	sst_drv_ctx = NULL;
	pci_release_regions(pci);
	pci_disable_device(pci);
	pci_set_drvdata(pci, NULL);
}

static void sst_save_dsp_context(void)
{
	struct snd_sst_ctxt_params fw_context;
	unsigned int pvt_id;
	struct ipc_post *msg = NULL;

	/*check cpu type*/
	if (sst_drv_ctx->pci_id == SST_MRST_PCI_ID)
		return;
	/*not supported for rest*/
	if (sst_drv_ctx->sst_state != SST_FW_RUNNING) {
		pr_debug("fw not running no context save ...\n");
		return;
	}

	/*send msg to fw*/
	if (sst_create_large_msg(&msg))
		return;
	pvt_id = sst_assign_pvt_id(sst_drv_ctx);
	sst_drv_ctx->alloc_block[0].sst_id = pvt_id;
	sst_drv_ctx->alloc_block[0].ops_block.condition = false;
	sst_drv_ctx->alloc_block[0].ops_block.on = true;
	sst_fill_header(&msg->header, IPC_IA_GET_FW_CTXT, 1, pvt_id);
	msg->header.part.data = sizeof(fw_context) + sizeof(u32);
	fw_context.address = virt_to_phys((void *)sst_drv_ctx->fw_cntx);
	fw_context.size = FW_CONTEXT_MEM;
	memcpy(msg->mailbox_data, &msg->header, sizeof(u32));
	memcpy(msg->mailbox_data + sizeof(u32),
				&fw_context, sizeof(fw_context));
	spin_lock(&sst_drv_ctx->list_spin_lock);
	list_add_tail(&msg->node, &sst_drv_ctx->ipc_dispatch_list);
	spin_unlock(&sst_drv_ctx->list_spin_lock);
	sst_post_message(&sst_drv_ctx->ipc_post_msg_wq);
	/*wait for reply*/
	if (sst_wait_timeout(sst_drv_ctx,
				&sst_drv_ctx->alloc_block[0].ops_block))
		pr_err("sst: err fw context save timeout  ...\n");
	sst_drv_ctx->alloc_block[0].sst_id = BLOCK_UNINIT;
	pr_debug("fw context saved  ...\n");
	return;
}

void intel_sst_pwm_suspend(unsigned int pwm_suspend)
{

	if (pwm_suspend) {
		pr_debug("SST_SND_DEVICE_SUSPEND doing rtpm_put\n");
		pm_runtime_put(&sst_drv_ctx->pci->dev);
	} else	{
		pr_debug("SST_SND_DEVICE_RESUME_SYNC\n");
		pm_runtime_get_sync(&sst_drv_ctx->pci->dev);
	}
}
EXPORT_SYMBOL(intel_sst_pwm_suspend);

int vibra_pwm_configure(unsigned int enable)
{
	union sst_pwmctrl_reg pwmctrl;

	if (enable) {

		/*1. Enable the PWM by setting PWM enable bit to 1 */
		pwmctrl.full = readl(sst_drv_ctx->shim + SST_PWMCTRL);
		pr_debug("Vibra:Read pwmctrl %x\n", pwmctrl.full);
		pwmctrl.part.pwmenable = 1;
		writel(pwmctrl.full, sst_drv_ctx->shim + SST_PWMCTRL);

		/*2. Read the PWM register to make sure there is no pending
		update.*/
		pwmctrl.full = readl(sst_drv_ctx->shim + SST_PWMCTRL);
		pr_debug("Read pwmctrl %x\n", pwmctrl.full);

		/*check pwnswupdate bit */
		if (pwmctrl.part.pwmswupdate)
			return -EBUSY;
		/*Base unit == 1*/
		pwmctrl.part.pwmswupdate = 0x1;
		pwmctrl.part.pwmbu = MAX_BASEUNIT;
		pwmctrl.part.pwmtd = MAX_TIMEDIVSOR;
		writel(pwmctrl.full,  sst_drv_ctx->shim + SST_PWMCTRL);
		pr_debug("Read pwmctrl %x\n", pwmctrl.full);
	} else { /*disable PWM block */
		   /*1. setting PWM enable bit to 0 */
		pwmctrl.full = readl(sst_drv_ctx->shim + SST_PWMCTRL);
		pwmctrl.part.pwmenable = 0;
		writel(pwmctrl.full,  sst_drv_ctx->shim + SST_PWMCTRL);
	}
	return 0;
}

/*
 * The runtime_suspend/resume is pretty much similar to the legacy
 * suspend/resume with the noted exception below: The PCI core takes care of
 * taking the system through D3hot and restoring it back to D0 and so there is
 * no need to duplicate that here.
 */
static int intel_sst_runtime_suspend(struct device *dev)
{
	union config_status_reg csr;

	pr_debug("runtime_suspend called\n");
	if (sst_drv_ctx->sst_state == SST_SUSPENDED) {
		pr_err("System already in Suspended state");
		return 0;
	}
	/*save fw context*/
	sst_save_dsp_context();
	/*Assert RESET on LPE Processor*/
	csr.full = sst_shim_read(sst_drv_ctx->shim, SST_CSR);
	sst_drv_ctx->csr_value = csr.full;
	csr.full = csr.full | 0x2;

	/* Move the SST state to Suspended */
	mutex_lock(&sst_drv_ctx->sst_lock);
	sst_drv_ctx->sst_state = SST_SUSPENDED;
	sst_shim_write(sst_drv_ctx->shim, SST_CSR, csr.full);
	mutex_unlock(&sst_drv_ctx->sst_lock);
	if (sst_drv_ctx->pci_id == SST_CLV_PCI_ID)
		vibra_pwm_configure(false);

	flush_workqueue(sst_drv_ctx->post_msg_wq);
	flush_workqueue(sst_drv_ctx->process_msg_wq);
	flush_workqueue(sst_drv_ctx->process_reply_wq);
	return 0;
}

static int intel_sst_runtime_resume(struct device *dev)
{
	u32 csr;

	pr_debug("runtime_resume called\n");
	if (sst_drv_ctx->sst_state != SST_SUSPENDED) {
		pr_err("SST is not in suspended state\n");
		return 0;
	}
	csr = sst_shim_read(sst_drv_ctx->shim, SST_CSR);
	/*
	 * To restore the csr_value after S0ix and S3 states.
	 * The value 0x30000 is to enable LPE dram high and low addresses.
	 * Reference:
	 * Penwell Audio Voice Module HAS 1.61 Section - 13.12.1 -
	 * CSR - Configuration and Status Register.
	 */
	csr |= (sst_drv_ctx->csr_value | 0x30000);
	sst_shim_write(sst_drv_ctx->shim, SST_CSR, csr);

	/* GPIO_PIN 12,13,74,75 needs to be configured in
	 * ALT_FUNC_2 mode for SSP3 IOs
	 */
	if (sst_drv_ctx->pci_id == SST_CLV_PCI_ID) {
		lnw_gpio_set_alt(CLV_I2S_3_CLK_GPIO_PIN, LNW_ALT_2);
		lnw_gpio_set_alt(CLV_I2S_3_FS_GPIO_PIN, LNW_ALT_2);
		lnw_gpio_set_alt(CLV_I2S_3_TXD_GPIO_PIN, LNW_ALT_2);
		lnw_gpio_set_alt(CLV_I2S_3_RXD_GPIO_PIN, LNW_ALT_2);
		lnw_gpio_set_alt(CLV_VIBRA_PWM_GPIO_PIN, LNW_ALT_2);

		vibra_pwm_configure(true);
	}

	sst_set_fw_state_locked(sst_drv_ctx, SST_UN_INIT);
	return 0;
}

static int intel_sst_runtime_idle(struct device *dev)
{
	pr_debug("runtime_idle called\n");
	if (!sst_drv_ctx->am_cnt && sst_drv_ctx->sst_state != SST_UN_INIT) {
		pm_schedule_suspend(dev, SST_SUSPEND_DELAY);
		return -EBUSY;
	} else {
		return 0;
	}
}

static const struct dev_pm_ops intel_sst_pm = {
	.suspend = intel_sst_runtime_suspend,
	.resume = intel_sst_runtime_resume,
	.runtime_suspend = intel_sst_runtime_suspend,
	.runtime_resume = intel_sst_runtime_resume,
	.runtime_idle = intel_sst_runtime_idle,
};

/* PCI Routines */
static DEFINE_PCI_DEVICE_TABLE(intel_sst_ids) = {
	{ PCI_VDEVICE(INTEL, SST_MRST_PCI_ID), 3},
	{ PCI_VDEVICE(INTEL, SST_MFLD_PCI_ID), 5},
	{ PCI_VDEVICE(INTEL, SST_CLV_PCI_ID), 3},
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, intel_sst_ids);

static struct pci_driver driver = {
	.name = SST_DRV_NAME,
	.id_table = intel_sst_ids,
	.probe = intel_sst_probe,
	.remove = __devexit_p(intel_sst_remove),
#ifdef CONFIG_PM
	.driver = {
		.pm = &intel_sst_pm,
	},
#endif
};

/**
* intel_sst_init - Module init function
*
* Registers with PCI
* Registers with /dev
* Init all data strutures
*/
static int __init intel_sst_init(void)
{
	/* Init all variables, data structure etc....*/
	int ret = 0;
	pr_info("INFO: ******** SST DRIVER loading.. Ver: %s\n",
				       SST_DRIVER_VERSION);

	mutex_init(&drv_ctx_lock);
	/* Register with PCI */
	ret = pci_register_driver(&driver);
	if (ret)
		pr_err("PCI register failed\n");
	return ret;
}

/**
* intel_sst_exit - Module exit function
*
* Unregisters with PCI
* Unregisters with /dev
* Frees all data strutures
*/
static void __exit intel_sst_exit(void)
{
	pci_unregister_driver(&driver);

	pr_debug("driver unloaded\n");
	sst_drv_ctx = NULL;
	return;
}

module_init_async(intel_sst_init);
module_exit(intel_sst_exit);
