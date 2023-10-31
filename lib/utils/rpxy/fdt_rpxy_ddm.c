/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) IPADS@SJTU 2023. All rights reserved.
 */

#include <sbi/sbi_error.h>
#include <sbi/sbi_heap.h>
#include <sbi/sbi_rpxy.h>
#include <libfdt.h>
#include <sbi_utils/fdt/fdt_helper.h>
#include <sbi_utils/rpxy/fdt_rpxy.h>
#include <sbi/sbi_domain.h>
#include <sbi_utils/mailbox/rpmi_msgprot.h>

struct ddm_rpxy_dispatcher {
	struct sbi_domain *dom;
	int (*setup)(void *fdt, int nodeoff,
			const struct fdt_match *match);
	int (*dispatch)(struct sbi_rpxy_service_group *grp, 
            struct sbi_rpxy_service *srv,
			void *tx, u32 tx_len,
			void *rx, u32 rx_len,
			unsigned long *ack_len);
};

#define MM_VERSION_MAJOR        1
#define MM_VERSION_MAJOR_SHIFT  16
#define MM_VERSION_MAJOR_MASK   0x7FFF
#define MM_VERSION_MINOR        0
#define MM_VERSION_MINOR_SHIFT  0
#define MM_VERSION_MINOR_MASK   0xFFFF
#define MM_VERSION_FORM(major, minor) ((major << MM_VERSION_MAJOR_SHIFT) | \
                                       (minor))
#define MM_VERSION_COMPILED     MM_VERSION_FORM(MM_VERSION_MAJOR, \
                                                MM_VERSION_MINOR)

struct efi_param_header {
	uint8_t type;	 /* type of the structure */
	uint8_t version; /* version of this structure */
	uint16_t size;	 /* size of this structure in bytes */
	uint32_t attr;	 /* attributes: unused bits SBZ */
};

struct efi_secure_partition_cpu_info {
	uint64_t mpidr;
	uint32_t linear_id;
	uint32_t flags;
};

struct efi_secure_partition_boot_info {
	struct efi_param_header header;
	uint64_t dd_mem_base;
	uint64_t dd_mem_limit;
	uint64_t dd_image_base;
	uint64_t dd_stack_base;
	uint64_t dd_heap_base;
	uint64_t dd_ns_comm_buf_base;
	uint64_t dd_shared_buf_base;
	uint64_t dd_image_size;
	uint64_t dd_pcpu_stack_size;
	uint64_t dd_heap_size;
	uint64_t dd_ns_comm_buf_size;
	uint64_t dd_shared_buf_size;
	uint32_t num_dd_mem_region;
	uint32_t num_cpus;
	struct efi_secure_partition_cpu_info *cpu_info;
};

struct efi_secure_shared_buffer {
	struct efi_secure_partition_boot_info mm_payload_boot_info;
	struct efi_secure_partition_cpu_info mm_cpu_info[1];
};

static void mm_setup_boot_info(uint64_t a1)
{
	struct efi_secure_shared_buffer *mm_shared_buffer = (void *)a1;
	struct efi_secure_partition_boot_info *mm_boot_info =
		&mm_shared_buffer->mm_payload_boot_info;
	mm_boot_info->header.version = 0x01;
	mm_boot_info->dd_mem_base    = 0x80C00000;
	mm_boot_info->dd_mem_limit   = 0x82000000;
	mm_boot_info->dd_image_base  = 0x80C00000;
	/* Stack from (dd_heap_base + dd_heap_size) to dd_shared_buf_base */
	mm_boot_info->dd_stack_base		   = 0x81F7FFFF;
	mm_boot_info->dd_heap_base		   = 0x80F00000;
	mm_boot_info->dd_ns_comm_buf_base	   = 0xFFE00000;
	mm_boot_info->dd_shared_buf_base	   = 0x81F80000;
	mm_boot_info->dd_image_size		   = 0x300000;
	mm_boot_info->dd_pcpu_stack_size	   = 0x10000;
	mm_boot_info->dd_heap_size		   = 0x800000;
	mm_boot_info->dd_ns_comm_buf_size	   = 0x200000;
	mm_boot_info->dd_shared_buf_size	   = 0x80000;
	mm_boot_info->num_dd_mem_region		   = 0x6;
	mm_boot_info->num_cpus			   = 1;
	mm_shared_buffer->mm_cpu_info[0].linear_id = 0;
	mm_shared_buffer->mm_cpu_info[0].flags	   = 0;
	mm_boot_info->cpu_info = mm_shared_buffer->mm_cpu_info;
}

int find_domain(void *fdt, int nodeoff, const char *compatible,
		struct sbi_domain **output_domain)
{
	u32 i;
	const u32 *val;
	struct sbi_domain *dom;
	int domain_offset, len;
	char name[64];

	val = fdt_getprop(fdt, nodeoff, compatible, &len);
	if (!val || len < 4) {
		return SBI_EINVAL;
	}

	domain_offset = fdt_node_offset_by_phandle(fdt, fdt32_to_cpu(*val));
	if (domain_offset < 0) {
		return SBI_EINVAL;
	}

    sbi_memset(name, 0, sizeof(name));

	/* Read DT node name and find match */
	strncpy(name, fdt_get_name(fdt, domain_offset, NULL), sizeof(name));
	name[sizeof(name) - 1] = '\0';

	sbi_domain_for_each(i, dom)
	{
		if (!sbi_strcmp(dom->name, name)) {
			*output_domain = dom;
			return SBI_SUCCESS;
		}
	}

	return SBI_EINVAL;
}

struct ddm_rpxy_dispatcher mm_dispatcher;

int ddm_mm_setup(void *fdt, int nodeoff, const struct fdt_match *match)
{
	if (find_domain(fdt, nodeoff, "opensbi-domain-instance",
			&mm_dispatcher.dom))
		return SBI_EINVAL;

	mm_setup_boot_info(mm_dispatcher.dom->next_arg1);

	return 0;
}

static int ddm_mm_dispatch(struct sbi_rpxy_service_group *grp,
				  struct sbi_rpxy_service *srv,
				  void *tx, u32 tx_len,
				  void *rx, u32 rx_len,
				  unsigned long *ack_len)
{
	int srv_id = srv->id;

	if (RPMI_MM_SRV_MM_VERSION == srv_id) {
		*((int32_t *)rx)		       = 0;
		*((uint32_t *)(rx + sizeof(uint32_t))) = MM_VERSION_COMPILED;
	} else if (RPMI_MM_SRV_MM_COMMUNICATE == srv_id) {
		sbi_dynamic_domain_entry(mm_dispatcher.dom->index);
	} else if (RPMI_MM_SRV_MM_COMPLETE == srv_id) {
		sbi_dynamic_domain_exit(0);
	}
	return 0;
}

struct ddm_rpxy_dispatcher mm_dispatcher = {
	.dom = NULL,
	.setup = ddm_mm_setup,
	.dispatch = ddm_mm_dispatch,
};

struct rpxy_ddm_data {
	u32 service_group_id;
	int num_services;
	struct sbi_rpxy_service *services;
    struct ddm_rpxy_dispatcher *srv_dispatcher;
};

static int rpxy_ddm_init(void *fdt, int nodeoff,
			  const struct fdt_match *match)
{
	int rc;
	struct sbi_rpxy_service_group *group;
	const struct rpxy_ddm_data *data = match->data;
    const struct ddm_rpxy_dispatcher *dispatcher = data->srv_dispatcher;

	/* Allocate context for RPXY mbox client */
	group = sbi_zalloc(sizeof(*group));
	if (!group)
		return SBI_ENOMEM;

	/* Setup DDM service group dispatcher */
	rc = dispatcher->setup(fdt, nodeoff, match);
	if (rc) {
		sbi_free(group);
		return 0;
	}

	/* Setup RPXY service group */
	group->transport_id = 0;
	group->service_group_id = data->service_group_id;
	group->max_message_data_len = -1;
	group->num_services = data->num_services;
	group->services = data->services;
	group->send_message = dispatcher->dispatch;

	/* Register RPXY service group */
	rc = sbi_rpxy_register_service_group(group);
	if (rc) {
		sbi_free(group);
		return rc;
	}

	return 0;
}

static struct sbi_rpxy_service mm_services[] = {
{
	.id = RPMI_MM_SRV_MM_VERSION,
	.min_tx_len = 0,
	.max_tx_len = 0,
	.min_rx_len = sizeof(u32),
	.max_rx_len = sizeof(u32),
},
{
	.id = RPMI_MM_SRV_MM_COMMUNICATE,
	.min_tx_len = 0,
	.max_tx_len = 0x1000,
	.min_rx_len = 0,
	.max_rx_len = 0,
},
{
	.id = RPMI_MM_SRV_MM_COMPLETE,
	.min_tx_len = 0,
	.max_tx_len = 0x1000,
	.min_rx_len = 0,
	.max_rx_len = 0,
},
};

static struct rpxy_ddm_data mm_data = {
	.service_group_id = RPMI_SRVGRP_DDM_MM,
	.num_services = array_size(mm_services),
	.services = mm_services,
    .srv_dispatcher = &mm_dispatcher,
};

static const struct fdt_match rpxy_ddm_match[] = {
	{ .compatible = "riscv,rpmi-ddm-mm", .data = &mm_data }, 
	{},
};

struct fdt_rpxy fdt_rpxy_ddm = {
	.match_table = rpxy_ddm_match,
	.init = rpxy_ddm_init,
};
