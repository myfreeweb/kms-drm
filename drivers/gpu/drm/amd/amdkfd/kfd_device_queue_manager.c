/*
 * Copyright 2014 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <linux/slab.h>
#include <linux/list.h>
#include <linux/types.h>
#include <linux/printk.h>
#include <linux/bitops.h>
#include <linux/sched.h>
#include "kfd_priv.h"
#include "kfd_device_queue_manager.h"
#include "kfd_mqd_manager.h"
#include "cik_regs.h"
#include "kfd_kernel_queue.h"

/* Size of the per-pipe EOP queue */
#define CIK_HPD_EOP_BYTES_LOG2 11
#define CIK_HPD_EOP_BYTES (1U << CIK_HPD_EOP_BYTES_LOG2)

static int set_pasid_vmid_mapping(struct device_queue_manager *dqm,
					unsigned int pasid, unsigned int vmid);

static int create_compute_queue_nocpsch(struct device_queue_manager *dqm,
					struct queue *q,
					struct qcm_process_device *qpd);

static int execute_queues_cpsch(struct device_queue_manager *dqm, bool lock);
static int destroy_queues_cpsch(struct device_queue_manager *dqm,
				bool preempt_static_queues, bool lock);

static int create_sdma_queue_nocpsch(struct device_queue_manager *dqm,
					struct queue *q,
					struct qcm_process_device *qpd);

static void deallocate_sdma_queue(struct device_queue_manager *dqm,
				unsigned int sdma_queue_id);

static inline
enum KFD_MQD_TYPE get_mqd_type_from_queue_type(enum kfd_queue_type type)
{
	if (type == KFD_QUEUE_TYPE_SDMA)
		return KFD_MQD_TYPE_SDMA;
	return KFD_MQD_TYPE_CP;
}

static bool is_pipe_enabled(struct device_queue_manager *dqm, int mec, int pipe)
{
	int i;
	int pipe_offset = mec * dqm->dev->shared_resources.num_pipe_per_mec
		+ pipe * dqm->dev->shared_resources.num_queue_per_pipe;

	/* queue is available for KFD usage if bit is 1 */
	for (i = 0; i <  dqm->dev->shared_resources.num_queue_per_pipe; ++i)
		if (test_bit(pipe_offset + i,
			      dqm->dev->shared_resources.queue_bitmap))
			return true;
	return false;
}

unsigned int get_queues_num(struct device_queue_manager *dqm)
{
	BUG_ON(!dqm || !dqm->dev);
	return bitmap_weight(dqm->dev->shared_resources.queue_bitmap,
				KGD_MAX_QUEUES);
}

unsigned int get_queues_per_pipe(struct device_queue_manager *dqm)
{
	BUG_ON(!dqm || !dqm->dev);
	return dqm->dev->shared_resources.num_queue_per_pipe;
}

unsigned int get_pipes_per_mec(struct device_queue_manager *dqm)
{
	BUG_ON(!dqm || !dqm->dev);
	return dqm->dev->shared_resources.num_pipe_per_mec;
}

void program_sh_mem_settings(struct device_queue_manager *dqm,
					struct qcm_process_device *qpd)
{
	return dqm->dev->kfd2kgd->program_sh_mem_settings(
						dqm->dev->kgd, qpd->vmid,
						qpd->sh_mem_config,
						qpd->sh_mem_ape1_base,
						qpd->sh_mem_ape1_limit,
						qpd->sh_mem_bases);
}

static int allocate_vmid(struct device_queue_manager *dqm,
			struct qcm_process_device *qpd,
			struct queue *q)
{
	int bit, allocated_vmid;

	if (dqm->vmid_bitmap == 0)
		return -ENOMEM;

	bit = find_first_bit((unsigned long *)&dqm->vmid_bitmap, CIK_VMID_NUM);
	clear_bit(bit, (unsigned long *)&dqm->vmid_bitmap);

	/* Kaveri kfd vmid's starts from vmid 8 */
	allocated_vmid = bit + KFD_VMID_START_OFFSET;
	pr_debug("vmid allocation %d\n", allocated_vmid);
	qpd->vmid = allocated_vmid;
	q->properties.vmid = allocated_vmid;

	set_pasid_vmid_mapping(dqm, q->process->pasid, q->properties.vmid);
	program_sh_mem_settings(dqm, qpd);

	return 0;
}

static void deallocate_vmid(struct device_queue_manager *dqm,
				struct qcm_process_device *qpd,
				struct queue *q)
{
	int bit = qpd->vmid - KFD_VMID_START_OFFSET;

	/* Release the vmid mapping */
	set_pasid_vmid_mapping(dqm, 0, qpd->vmid);

	set_bit(bit, (unsigned long *)&dqm->vmid_bitmap);
	qpd->vmid = 0;
	q->properties.vmid = 0;
}

static int create_queue_nocpsch(struct device_queue_manager *dqm,
				struct queue *q,
				struct qcm_process_device *qpd,
				int *allocated_vmid)
{
	int retval;

	BUG_ON(!dqm || !q || !qpd || !allocated_vmid);

	print_queue(q);

	mutex_lock(&dqm->lock);

	if (dqm->total_queue_count >= max_num_of_queues_per_device) {
		pr_warn("Can't create new usermode queue because %d queues were already created\n",
				dqm->total_queue_count);
		mutex_unlock(&dqm->lock);
		return -EPERM;
	}

	if (list_empty(&qpd->queues_list)) {
		retval = allocate_vmid(dqm, qpd, q);
		if (retval != 0) {
			mutex_unlock(&dqm->lock);
			return retval;
		}
	}
	*allocated_vmid = qpd->vmid;
	q->properties.vmid = qpd->vmid;

	if (q->properties.type == KFD_QUEUE_TYPE_COMPUTE)
		retval = create_compute_queue_nocpsch(dqm, q, qpd);
	if (q->properties.type == KFD_QUEUE_TYPE_SDMA)
		retval = create_sdma_queue_nocpsch(dqm, q, qpd);

	if (retval != 0) {
		if (list_empty(&qpd->queues_list)) {
			deallocate_vmid(dqm, qpd, q);
			*allocated_vmid = 0;
		}
		mutex_unlock(&dqm->lock);
		return retval;
	}

	list_add(&q->list, &qpd->queues_list);
	if (q->properties.is_active)
		dqm->queue_count++;

	if (q->properties.type == KFD_QUEUE_TYPE_SDMA)
		dqm->sdma_queue_count++;

	/*
	 * Unconditionally increment this counter, regardless of the queue's
	 * type or whether the queue is active.
	 */
	dqm->total_queue_count++;
	pr_debug("Total of %d queues are accountable so far\n",
			dqm->total_queue_count);

	mutex_unlock(&dqm->lock);
	return 0;
}

static int allocate_hqd(struct device_queue_manager *dqm, struct queue *q)
{
	bool set;
	int pipe, bit, i;

	set = false;

	for (pipe = dqm->next_pipe_to_allocate, i = 0;
			i < get_pipes_per_mec(dqm);
			pipe = ((pipe + 1) % get_pipes_per_mec(dqm)), ++i) {

		if (!is_pipe_enabled(dqm, 0, pipe))
			continue;

		if (dqm->allocated_queues[pipe] != 0) {
			bit = find_first_bit(
				(unsigned long *)&dqm->allocated_queues[pipe],
				get_queues_per_pipe(dqm));

			clear_bit(bit,
				(unsigned long *)&dqm->allocated_queues[pipe]);
			q->pipe = pipe;
			q->queue = bit;
			set = true;
			break;
		}
	}

	if (!set)
		return -EBUSY;

	pr_debug("hqd slot - pipe %d, queue %d\n", q->pipe, q->queue);
	/* horizontal hqd allocation */
	dqm->next_pipe_to_allocate = (pipe + 1) % get_pipes_per_mec(dqm);

	return 0;
}

static inline void deallocate_hqd(struct device_queue_manager *dqm,
				struct queue *q)
{
	set_bit(q->queue, (unsigned long *)&dqm->allocated_queues[q->pipe]);
}

static int create_compute_queue_nocpsch(struct device_queue_manager *dqm,
					struct queue *q,
					struct qcm_process_device *qpd)
{
	int retval;
	struct mqd_manager *mqd;

	BUG_ON(!dqm || !q || !qpd);

	mqd = dqm->ops.get_mqd_manager(dqm, KFD_MQD_TYPE_COMPUTE);
	if (mqd == NULL)
		return -ENOMEM;

	retval = allocate_hqd(dqm, q);
	if (retval != 0)
		return retval;

	retval = mqd->init_mqd(mqd, &q->mqd, &q->mqd_mem_obj,
				&q->gart_mqd_addr, &q->properties);
	if (retval != 0) {
		deallocate_hqd(dqm, q);
		return retval;
	}

	pr_debug("Loading mqd to hqd on pipe %d, queue %d\n",
			q->pipe, q->queue);

	retval = mqd->load_mqd(mqd, q->mqd, q->pipe,
			q->queue, (uint32_t __user *) q->properties.write_ptr);
	if (retval != 0) {
		deallocate_hqd(dqm, q);
		mqd->uninit_mqd(mqd, q->mqd, q->mqd_mem_obj);
		return retval;
	}

	return 0;
}

static int destroy_queue_nocpsch(struct device_queue_manager *dqm,
				struct qcm_process_device *qpd,
				struct queue *q)
{
	int retval;
	struct mqd_manager *mqd;

	BUG_ON(!dqm || !q || !q->mqd || !qpd);

	retval = 0;

	mutex_lock(&dqm->lock);

	if (q->properties.type == KFD_QUEUE_TYPE_COMPUTE) {
		mqd = dqm->ops.get_mqd_manager(dqm, KFD_MQD_TYPE_COMPUTE);
		if (mqd == NULL) {
			retval = -ENOMEM;
			goto out;
		}
		deallocate_hqd(dqm, q);
	} else if (q->properties.type == KFD_QUEUE_TYPE_SDMA) {
		mqd = dqm->ops.get_mqd_manager(dqm, KFD_MQD_TYPE_SDMA);
		if (mqd == NULL) {
			retval = -ENOMEM;
			goto out;
		}
		dqm->sdma_queue_count--;
		deallocate_sdma_queue(dqm, q->sdma_id);
	} else {
		pr_debug("q->properties.type %d is invalid\n",
				q->properties.type);
		retval = -EINVAL;
		goto out;
	}

	retval = mqd->destroy_mqd(mqd, q->mqd,
				KFD_PREEMPT_TYPE_WAVEFRONT_RESET,
				QUEUE_PREEMPT_DEFAULT_TIMEOUT_MS,
				q->pipe, q->queue);

	if (retval != 0)
		goto out;

	mqd->uninit_mqd(mqd, q->mqd, q->mqd_mem_obj);

	list_del(&q->list);
	if (list_empty(&qpd->queues_list))
		deallocate_vmid(dqm, qpd, q);
	if (q->properties.is_active)
		dqm->queue_count--;

	/*
	 * Unconditionally decrement this counter, regardless of the queue's
	 * type
	 */
	dqm->total_queue_count--;
	pr_debug("Total of %d queues are accountable so far\n",
			dqm->total_queue_count);

out:
	mutex_unlock(&dqm->lock);
	return retval;
}

static int update_queue(struct device_queue_manager *dqm, struct queue *q)
{
	int retval;
	struct mqd_manager *mqd;
	bool prev_active = false;

	BUG_ON(!dqm || !q || !q->mqd);

	mutex_lock(&dqm->lock);
	mqd = dqm->ops.get_mqd_manager(dqm,
			get_mqd_type_from_queue_type(q->properties.type));
	if (mqd == NULL) {
		mutex_unlock(&dqm->lock);
		return -ENOMEM;
	}

	if (q->properties.is_active)
		prev_active = true;

	/*
	 *
	 * check active state vs. the previous state
	 * and modify counter accordingly
	 */
	retval = mqd->update_mqd(mqd, q->mqd, &q->properties);
	if ((q->properties.is_active) && (!prev_active))
		dqm->queue_count++;
	else if ((!q->properties.is_active) && (prev_active))
		dqm->queue_count--;

	if (sched_policy != KFD_SCHED_POLICY_NO_HWS)
		retval = execute_queues_cpsch(dqm, false);

	mutex_unlock(&dqm->lock);
	return retval;
}

static struct mqd_manager *get_mqd_manager_nocpsch(
		struct device_queue_manager *dqm, enum KFD_MQD_TYPE type)
{
	struct mqd_manager *mqd;

	BUG_ON(!dqm || type >= KFD_MQD_TYPE_MAX);

	pr_debug("mqd type %d\n", type);

	mqd = dqm->mqds[type];
	if (!mqd) {
		mqd = mqd_manager_init(type, dqm->dev);
		if (mqd == NULL)
			pr_err("mqd manager is NULL");
		dqm->mqds[type] = mqd;
	}

	return mqd;
}

static int register_process_nocpsch(struct device_queue_manager *dqm,
					struct qcm_process_device *qpd)
{
	struct device_process_node *n;
	int retval;

	BUG_ON(!dqm || !qpd);

	n = kzalloc(sizeof(struct device_process_node), GFP_KERNEL);
	if (!n)
		return -ENOMEM;

	n->qpd = qpd;

	mutex_lock(&dqm->lock);
	list_add(&n->list, &dqm->queues);

	retval = dqm->ops_asic_specific.register_process(dqm, qpd);

	dqm->processes_count++;

	mutex_unlock(&dqm->lock);

	return retval;
}

static int unregister_process_nocpsch(struct device_queue_manager *dqm,
					struct qcm_process_device *qpd)
{
	int retval;
	struct device_process_node *cur, *next;

	BUG_ON(!dqm || !qpd);

	pr_debug("qpd->queues_list is %s\n",
			list_empty(&qpd->queues_list) ? "empty" : "not empty");

	retval = 0;
	mutex_lock(&dqm->lock);

	list_for_each_entry_safe(cur, next, &dqm->queues, list) {
		if (qpd == cur->qpd) {
			list_del(&cur->list);
			kfree(cur);
			dqm->processes_count--;
			goto out;
		}
	}
	/* qpd not found in dqm list */
	retval = 1;
out:
	mutex_unlock(&dqm->lock);
	return retval;
}

static int
set_pasid_vmid_mapping(struct device_queue_manager *dqm, unsigned int pasid,
			unsigned int vmid)
{
	uint32_t pasid_mapping;

	pasid_mapping = (pasid == 0) ? 0 :
		(uint32_t)pasid |
		ATC_VMID_PASID_MAPPING_VALID;

	return dqm->dev->kfd2kgd->set_pasid_vmid_mapping(
						dqm->dev->kgd, pasid_mapping,
						vmid);
}

static void init_interrupts(struct device_queue_manager *dqm)
{
	unsigned int i;

	BUG_ON(dqm == NULL);

	for (i = 0 ; i < get_pipes_per_mec(dqm) ; i++)
		if (is_pipe_enabled(dqm, 0, i))
			dqm->dev->kfd2kgd->init_interrupts(dqm->dev->kgd, i);
}

static int initialize_nocpsch(struct device_queue_manager *dqm)
{
	int pipe, queue;

	BUG_ON(!dqm);

	pr_debug("num of pipes: %d\n", get_pipes_per_mec(dqm));

	mutex_init(&dqm->lock);
	INIT_LIST_HEAD(&dqm->queues);
	dqm->queue_count = dqm->next_pipe_to_allocate = 0;
	dqm->sdma_queue_count = 0;
	dqm->allocated_queues = kcalloc(get_pipes_per_mec(dqm),
					sizeof(unsigned int), GFP_KERNEL);
	if (!dqm->allocated_queues) {
		mutex_destroy(&dqm->lock);
		return -ENOMEM;
	}

	for (pipe = 0; pipe < get_pipes_per_mec(dqm); pipe++) {
		int pipe_offset = pipe * get_queues_per_pipe(dqm);

		for (queue = 0; queue < get_queues_per_pipe(dqm); queue++)
			if (test_bit(pipe_offset + queue,
				     dqm->dev->shared_resources.queue_bitmap))
				dqm->allocated_queues[pipe] |= 1 << queue;
	}

	dqm->vmid_bitmap = (1 << VMID_PER_DEVICE) - 1;
	dqm->sdma_bitmap = (1 << CIK_SDMA_QUEUES) - 1;

	return 0;
}

static void uninitialize_nocpsch(struct device_queue_manager *dqm)
{
	int i;

	BUG_ON(!dqm);

	BUG_ON(dqm->queue_count > 0 || dqm->processes_count > 0);

	kfree(dqm->allocated_queues);
	for (i = 0 ; i < KFD_MQD_TYPE_MAX ; i++)
		kfree(dqm->mqds[i]);
	mutex_destroy(&dqm->lock);
	kfd_gtt_sa_free(dqm->dev, dqm->pipeline_mem);
}

static int start_nocpsch(struct device_queue_manager *dqm)
{
	init_interrupts(dqm);
	return 0;
}

static int stop_nocpsch(struct device_queue_manager *dqm)
{
	return 0;
}

static int allocate_sdma_queue(struct device_queue_manager *dqm,
				unsigned int *sdma_queue_id)
{
	int bit;

	if (dqm->sdma_bitmap == 0)
		return -ENOMEM;

	bit = find_first_bit((unsigned long *)&dqm->sdma_bitmap,
				CIK_SDMA_QUEUES);

	clear_bit(bit, (unsigned long *)&dqm->sdma_bitmap);
	*sdma_queue_id = bit;

	return 0;
}

static void deallocate_sdma_queue(struct device_queue_manager *dqm,
				unsigned int sdma_queue_id)
{
	if (sdma_queue_id >= CIK_SDMA_QUEUES)
		return;
	set_bit(sdma_queue_id, (unsigned long *)&dqm->sdma_bitmap);
}

static int create_sdma_queue_nocpsch(struct device_queue_manager *dqm,
					struct queue *q,
					struct qcm_process_device *qpd)
{
	struct mqd_manager *mqd;
	int retval;

	mqd = dqm->ops.get_mqd_manager(dqm, KFD_MQD_TYPE_SDMA);
	if (!mqd)
		return -ENOMEM;

	retval = allocate_sdma_queue(dqm, &q->sdma_id);
	if (retval != 0)
		return retval;

	q->properties.sdma_queue_id = q->sdma_id % CIK_SDMA_QUEUES_PER_ENGINE;
	q->properties.sdma_engine_id = q->sdma_id / CIK_SDMA_ENGINE_NUM;

	pr_debug("SDMA id is:    %d\n", q->sdma_id);
	pr_debug("SDMA queue id: %d\n", q->properties.sdma_queue_id);
	pr_debug("SDMA engine id: %d\n", q->properties.sdma_engine_id);

	dqm->ops_asic_specific.init_sdma_vm(dqm, q, qpd);
	retval = mqd->init_mqd(mqd, &q->mqd, &q->mqd_mem_obj,
				&q->gart_mqd_addr, &q->properties);
	if (retval != 0) {
		deallocate_sdma_queue(dqm, q->sdma_id);
		return retval;
	}

	retval = mqd->load_mqd(mqd, q->mqd, 0,
				0, NULL);
	if (retval != 0) {
		deallocate_sdma_queue(dqm, q->sdma_id);
		mqd->uninit_mqd(mqd, q->mqd, q->mqd_mem_obj);
		return retval;
	}

	return 0;
}

/*
 * Device Queue Manager implementation for cp scheduler
 */

static int set_sched_resources(struct device_queue_manager *dqm)
{
	int i, mec;
	struct scheduling_resources res;

	BUG_ON(!dqm);

	res.vmid_mask = (1 << VMID_PER_DEVICE) - 1;
	res.vmid_mask <<= KFD_VMID_START_OFFSET;

	res.queue_mask = 0;
	for (i = 0; i < KGD_MAX_QUEUES; ++i) {
		mec = (i / dqm->dev->shared_resources.num_queue_per_pipe)
			/ dqm->dev->shared_resources.num_pipe_per_mec;

		if (!test_bit(i, dqm->dev->shared_resources.queue_bitmap))
			continue;

		/* only acquire queues from the first MEC */
		if (mec > 0)
			continue;

		/* This situation may be hit in the future if a new HW
		 * generation exposes more than 64 queues. If so, the
		 * definition of res.queue_mask needs updating
		 */
		if (WARN_ON(i >= (sizeof(res.queue_mask)*8))) {
			pr_err("Invalid queue enabled by amdgpu: %d\n", i);
			break;
		}

		res.queue_mask |= (1ull << i);
	}
	res.gws_mask = res.oac_mask = res.gds_heap_base =
						res.gds_heap_size = 0;

	pr_debug("Scheduling resources:\n"
			"vmid mask: 0x%8X\n"
			"queue mask: 0x%8llX\n",
			res.vmid_mask, res.queue_mask);

	return pm_send_set_resources(&dqm->packets, &res);
}

static int initialize_cpsch(struct device_queue_manager *dqm)
{
	int retval;

	BUG_ON(!dqm);

	pr_debug("num of pipes: %d\n", get_pipes_per_mec(dqm));

	mutex_init(&dqm->lock);
	INIT_LIST_HEAD(&dqm->queues);
	dqm->queue_count = dqm->processes_count = 0;
	dqm->sdma_queue_count = 0;
	dqm->active_runlist = false;
	retval = dqm->ops_asic_specific.initialize(dqm);
	if (retval != 0)
		goto fail_init_pipelines;

	return 0;

fail_init_pipelines:
	mutex_destroy(&dqm->lock);
	return retval;
}

static int start_cpsch(struct device_queue_manager *dqm)
{
	struct device_process_node *node;
	int retval;

	BUG_ON(!dqm);

	retval = 0;

	retval = pm_init(&dqm->packets, dqm);
	if (retval != 0)
		goto fail_packet_manager_init;

	retval = set_sched_resources(dqm);
	if (retval != 0)
		goto fail_set_sched_resources;

	pr_debug("Allocating fence memory\n");

	/* allocate fence memory on the gart */
	retval = kfd_gtt_sa_allocate(dqm->dev, sizeof(*dqm->fence_addr),
					&dqm->fence_mem);

	if (retval != 0)
		goto fail_allocate_vidmem;

	dqm->fence_addr = dqm->fence_mem->cpu_ptr;
	dqm->fence_gpu_addr = dqm->fence_mem->gpu_addr;

	init_interrupts(dqm);

	list_for_each_entry(node, &dqm->queues, list)
		if (node->qpd->pqm->process && dqm->dev)
			kfd_bind_process_to_device(dqm->dev,
						node->qpd->pqm->process);

	execute_queues_cpsch(dqm, true);

	return 0;
fail_allocate_vidmem:
fail_set_sched_resources:
	pm_uninit(&dqm->packets);
fail_packet_manager_init:
	return retval;
}

static int stop_cpsch(struct device_queue_manager *dqm)
{
	struct device_process_node *node;
	struct kfd_process_device *pdd;

	BUG_ON(!dqm);

	destroy_queues_cpsch(dqm, true, true);

	list_for_each_entry(node, &dqm->queues, list) {
		pdd = qpd_to_pdd(node->qpd);
		pdd->bound = false;
	}
	kfd_gtt_sa_free(dqm->dev, dqm->fence_mem);
	pm_uninit(&dqm->packets);

	return 0;
}

static int create_kernel_queue_cpsch(struct device_queue_manager *dqm,
					struct kernel_queue *kq,
					struct qcm_process_device *qpd)
{
	BUG_ON(!dqm || !kq || !qpd);

	mutex_lock(&dqm->lock);
	if (dqm->total_queue_count >= max_num_of_queues_per_device) {
		pr_warn("Can't create new kernel queue because %d queues were already created\n",
				dqm->total_queue_count);
		mutex_unlock(&dqm->lock);
		return -EPERM;
	}

	/*
	 * Unconditionally increment this counter, regardless of the queue's
	 * type or whether the queue is active.
	 */
	dqm->total_queue_count++;
	pr_debug("Total of %d queues are accountable so far\n",
			dqm->total_queue_count);

	list_add(&kq->list, &qpd->priv_queue_list);
	dqm->queue_count++;
	qpd->is_debug = true;
	execute_queues_cpsch(dqm, false);
	mutex_unlock(&dqm->lock);

	return 0;
}

static void destroy_kernel_queue_cpsch(struct device_queue_manager *dqm,
					struct kernel_queue *kq,
					struct qcm_process_device *qpd)
{
	BUG_ON(!dqm || !kq);

	mutex_lock(&dqm->lock);
	/* here we actually preempt the DIQ */
	destroy_queues_cpsch(dqm, true, false);
	list_del(&kq->list);
	dqm->queue_count--;
	qpd->is_debug = false;
	execute_queues_cpsch(dqm, false);
	/*
	 * Unconditionally decrement this counter, regardless of the queue's
	 * type.
	 */
	dqm->total_queue_count--;
	pr_debug("Total of %d queues are accountable so far\n",
			dqm->total_queue_count);
	mutex_unlock(&dqm->lock);
}

static void select_sdma_engine_id(struct queue *q)
{
	static int sdma_id;

	q->sdma_id = sdma_id;
	sdma_id = (sdma_id + 1) % 2;
}

static int create_queue_cpsch(struct device_queue_manager *dqm, struct queue *q,
			struct qcm_process_device *qpd, int *allocate_vmid)
{
	int retval;
	struct mqd_manager *mqd;

	BUG_ON(!dqm || !q || !qpd);

	retval = 0;

	if (allocate_vmid)
		*allocate_vmid = 0;

	mutex_lock(&dqm->lock);

	if (dqm->total_queue_count >= max_num_of_queues_per_device) {
		pr_warn("Can't create new usermode queue because %d queues were already created\n",
				dqm->total_queue_count);
		retval = -EPERM;
		goto out;
	}

	if (q->properties.type == KFD_QUEUE_TYPE_SDMA)
		select_sdma_engine_id(q);

	mqd = dqm->ops.get_mqd_manager(dqm,
			get_mqd_type_from_queue_type(q->properties.type));

	if (mqd == NULL) {
		mutex_unlock(&dqm->lock);
		return -ENOMEM;
	}

	dqm->ops_asic_specific.init_sdma_vm(dqm, q, qpd);
	retval = mqd->init_mqd(mqd, &q->mqd, &q->mqd_mem_obj,
				&q->gart_mqd_addr, &q->properties);
	if (retval != 0)
		goto out;

	list_add(&q->list, &qpd->queues_list);
	if (q->properties.is_active) {
		dqm->queue_count++;
		retval = execute_queues_cpsch(dqm, false);
	}

	if (q->properties.type == KFD_QUEUE_TYPE_SDMA)
		dqm->sdma_queue_count++;
	/*
	 * Unconditionally increment this counter, regardless of the queue's
	 * type or whether the queue is active.
	 */
	dqm->total_queue_count++;

	pr_debug("Total of %d queues are accountable so far\n",
			dqm->total_queue_count);

out:
	mutex_unlock(&dqm->lock);
	return retval;
}

int amdkfd_fence_wait_timeout(unsigned int *fence_addr,
				unsigned int fence_value,
				unsigned long timeout)
{
	BUG_ON(!fence_addr);
	timeout += jiffies;

	while (*fence_addr != fence_value) {
		if (time_after(jiffies, timeout)) {
			pr_err("qcm fence wait loop timeout expired\n");
			return -ETIME;
		}
		schedule();
	}

	return 0;
}

static int destroy_sdma_queues(struct device_queue_manager *dqm,
				unsigned int sdma_engine)
{
	return pm_send_unmap_queue(&dqm->packets, KFD_QUEUE_TYPE_SDMA,
			KFD_PREEMPT_TYPE_FILTER_DYNAMIC_QUEUES, 0, false,
			sdma_engine);
}

static int destroy_queues_cpsch(struct device_queue_manager *dqm,
				bool preempt_static_queues, bool lock)
{
	int retval;
	enum kfd_preempt_type_filter preempt_type;
	struct kfd_process_device *pdd;

	BUG_ON(!dqm);

	retval = 0;

	if (lock)
		mutex_lock(&dqm->lock);
	if (!dqm->active_runlist)
		goto out;

	pr_debug("Before destroying queues, sdma queue count is : %u\n",
		dqm->sdma_queue_count);

	if (dqm->sdma_queue_count > 0) {
		destroy_sdma_queues(dqm, 0);
		destroy_sdma_queues(dqm, 1);
	}

	preempt_type = preempt_static_queues ?
			KFD_PREEMPT_TYPE_FILTER_ALL_QUEUES :
			KFD_PREEMPT_TYPE_FILTER_DYNAMIC_QUEUES;

	retval = pm_send_unmap_queue(&dqm->packets, KFD_QUEUE_TYPE_COMPUTE,
			preempt_type, 0, false, 0);
	if (retval != 0)
		goto out;

	*dqm->fence_addr = KFD_FENCE_INIT;
	pm_send_query_status(&dqm->packets, dqm->fence_gpu_addr,
				KFD_FENCE_COMPLETED);
	/* should be timed out */
	retval = amdkfd_fence_wait_timeout(dqm->fence_addr, KFD_FENCE_COMPLETED,
				QUEUE_PREEMPT_DEFAULT_TIMEOUT_MS);
	if (retval != 0) {
		pdd = kfd_get_process_device_data(dqm->dev,
				kfd_get_process(current));
		pdd->reset_wavefronts = true;
		goto out;
	}
	pm_release_ib(&dqm->packets);
	dqm->active_runlist = false;

out:
	if (lock)
		mutex_unlock(&dqm->lock);
	return retval;
}

static int execute_queues_cpsch(struct device_queue_manager *dqm, bool lock)
{
	int retval;

	BUG_ON(!dqm);

	if (lock)
		mutex_lock(&dqm->lock);

	retval = destroy_queues_cpsch(dqm, false, false);
	if (retval != 0) {
		pr_err("The cp might be in an unrecoverable state due to an unsuccessful queues preemption");
		goto out;
	}

	if (dqm->queue_count <= 0 || dqm->processes_count <= 0) {
		retval = 0;
		goto out;
	}

	if (dqm->active_runlist) {
		retval = 0;
		goto out;
	}

	retval = pm_send_runlist(&dqm->packets, &dqm->queues);
	if (retval != 0) {
		pr_err("failed to execute runlist");
		goto out;
	}
	dqm->active_runlist = true;

out:
	if (lock)
		mutex_unlock(&dqm->lock);
	return retval;
}

static int destroy_queue_cpsch(struct device_queue_manager *dqm,
				struct qcm_process_device *qpd,
				struct queue *q)
{
	int retval;
	struct mqd_manager *mqd;
	bool preempt_all_queues;

	BUG_ON(!dqm || !qpd || !q);

	preempt_all_queues = false;

	retval = 0;

	/* remove queue from list to prevent rescheduling after preemption */
	mutex_lock(&dqm->lock);

	if (qpd->is_debug) {
		/*
		 * error, currently we do not allow to destroy a queue
		 * of a currently debugged process
		 */
		retval = -EBUSY;
		goto failed_try_destroy_debugged_queue;

	}

	mqd = dqm->ops.get_mqd_manager(dqm,
			get_mqd_type_from_queue_type(q->properties.type));
	if (!mqd) {
		retval = -ENOMEM;
		goto failed;
	}

	if (q->properties.type == KFD_QUEUE_TYPE_SDMA)
		dqm->sdma_queue_count--;

	list_del(&q->list);
	if (q->properties.is_active)
		dqm->queue_count--;

	execute_queues_cpsch(dqm, false);

	mqd->uninit_mqd(mqd, q->mqd, q->mqd_mem_obj);

	/*
	 * Unconditionally decrement this counter, regardless of the queue's
	 * type
	 */
	dqm->total_queue_count--;
	pr_debug("Total of %d queues are accountable so far\n",
			dqm->total_queue_count);

	mutex_unlock(&dqm->lock);

	return 0;

failed:
failed_try_destroy_debugged_queue:

	mutex_unlock(&dqm->lock);
	return retval;
}

/*
 * Low bits must be 0000/FFFF as required by HW, high bits must be 0 to
 * stay in user mode.
 */
#define APE1_FIXED_BITS_MASK 0xFFFF80000000FFFFULL
/* APE1 limit is inclusive and 64K aligned. */
#define APE1_LIMIT_ALIGNMENT 0xFFFF

static bool set_cache_memory_policy(struct device_queue_manager *dqm,
				   struct qcm_process_device *qpd,
				   enum cache_policy default_policy,
				   enum cache_policy alternate_policy,
				   void __user *alternate_aperture_base,
				   uint64_t alternate_aperture_size)
{
	bool retval;

	mutex_lock(&dqm->lock);

	if (alternate_aperture_size == 0) {
		/* base > limit disables APE1 */
		qpd->sh_mem_ape1_base = 1;
		qpd->sh_mem_ape1_limit = 0;
	} else {
		/*
		 * In FSA64, APE1_Base[63:0] = { 16{SH_MEM_APE1_BASE[31]},
		 *			SH_MEM_APE1_BASE[31:0], 0x0000 }
		 * APE1_Limit[63:0] = { 16{SH_MEM_APE1_LIMIT[31]},
		 *			SH_MEM_APE1_LIMIT[31:0], 0xFFFF }
		 * Verify that the base and size parameters can be
		 * represented in this format and convert them.
		 * Additionally restrict APE1 to user-mode addresses.
		 */

		uint64_t base = (uintptr_t)alternate_aperture_base;
		uint64_t limit = base + alternate_aperture_size - 1;

		if (limit <= base)
			goto out;

		if ((base & APE1_FIXED_BITS_MASK) != 0)
			goto out;

		if ((limit & APE1_FIXED_BITS_MASK) != APE1_LIMIT_ALIGNMENT)
			goto out;

		qpd->sh_mem_ape1_base = base >> 16;
		qpd->sh_mem_ape1_limit = limit >> 16;
	}

	retval = dqm->ops_asic_specific.set_cache_memory_policy(
			dqm,
			qpd,
			default_policy,
			alternate_policy,
			alternate_aperture_base,
			alternate_aperture_size);

	if ((sched_policy == KFD_SCHED_POLICY_NO_HWS) && (qpd->vmid != 0))
		program_sh_mem_settings(dqm, qpd);

	pr_debug("sh_mem_config: 0x%x, ape1_base: 0x%x, ape1_limit: 0x%x\n",
		qpd->sh_mem_config, qpd->sh_mem_ape1_base,
		qpd->sh_mem_ape1_limit);

	mutex_unlock(&dqm->lock);
	return retval;

out:
	mutex_unlock(&dqm->lock);
	return false;
}

struct device_queue_manager *device_queue_manager_init(struct kfd_dev *dev)
{
	struct device_queue_manager *dqm;

	BUG_ON(!dev);

	pr_debug("Loading device queue manager\n");

	dqm = kzalloc(sizeof(struct device_queue_manager), GFP_KERNEL);
	if (!dqm)
		return NULL;

	dqm->dev = dev;
	switch (sched_policy) {
	case KFD_SCHED_POLICY_HWS:
	case KFD_SCHED_POLICY_HWS_NO_OVERSUBSCRIPTION:
		/* initialize dqm for cp scheduling */
		dqm->ops.create_queue = create_queue_cpsch;
		dqm->ops.initialize = initialize_cpsch;
		dqm->ops.start = start_cpsch;
		dqm->ops.stop = stop_cpsch;
		dqm->ops.destroy_queue = destroy_queue_cpsch;
		dqm->ops.update_queue = update_queue;
		dqm->ops.get_mqd_manager = get_mqd_manager_nocpsch;
		dqm->ops.register_process = register_process_nocpsch;
		dqm->ops.unregister_process = unregister_process_nocpsch;
		dqm->ops.uninitialize = uninitialize_nocpsch;
		dqm->ops.create_kernel_queue = create_kernel_queue_cpsch;
		dqm->ops.destroy_kernel_queue = destroy_kernel_queue_cpsch;
		dqm->ops.set_cache_memory_policy = set_cache_memory_policy;
		break;
	case KFD_SCHED_POLICY_NO_HWS:
		/* initialize dqm for no cp scheduling */
		dqm->ops.start = start_nocpsch;
		dqm->ops.stop = stop_nocpsch;
		dqm->ops.create_queue = create_queue_nocpsch;
		dqm->ops.destroy_queue = destroy_queue_nocpsch;
		dqm->ops.update_queue = update_queue;
		dqm->ops.get_mqd_manager = get_mqd_manager_nocpsch;
		dqm->ops.register_process = register_process_nocpsch;
		dqm->ops.unregister_process = unregister_process_nocpsch;
		dqm->ops.initialize = initialize_nocpsch;
		dqm->ops.uninitialize = uninitialize_nocpsch;
		dqm->ops.set_cache_memory_policy = set_cache_memory_policy;
		break;
	default:
		BUG();
		break;
	}

	switch (dev->device_info->asic_family) {
	case CHIP_CARRIZO:
		device_queue_manager_init_vi(&dqm->ops_asic_specific);
		break;

	case CHIP_KAVERI:
		device_queue_manager_init_cik(&dqm->ops_asic_specific);
		break;
	}

	if (dqm->ops.initialize(dqm) != 0) {
		kfree(dqm);
		return NULL;
	}

	return dqm;
}

void device_queue_manager_uninit(struct device_queue_manager *dqm)
{
	BUG_ON(!dqm);

	dqm->ops.uninitialize(dqm);
	kfree(dqm);
}
