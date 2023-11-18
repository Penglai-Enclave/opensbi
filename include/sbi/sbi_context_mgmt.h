/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) IPADS@SJTU 2023. All rights reserved.
 */

#ifndef __SBI_CONTEXT_H__
#define __SBI_CONTEXT_H__

#include <sbi/sbi_types.h>
#include <sbi/sbi_trap.h>
#include <sbi/sbi_domain.h>

/** Context representation for a hart within a domain */
struct sbi_context {
	/** Trap-related states such as GPRs, mepc, and mstatus */
	struct sbi_trap_regs regs;

	/** Supervisor trap vector base address register */
	uint64_t csr_stvec;
	/** Supervisor scratch register for temporary storage */
	uint64_t csr_sscratch;
	/** Supervisor interrupt enable register */
	uint64_t csr_sie;
	/** Supervisor interrupt pending register */
	uint64_t csr_sip;
	/** Supervisor address translation and protection register */
	uint64_t csr_satp;

	/** Reference to the owning domain */
	struct sbi_domain *dom;
	/** Next context to jump to during context exits */
	struct sbi_context *next_ctx;
	/** Is context initialized and idle */
	bool initialized;
};

/** Get the context pointer for a given hart index and domain */
#define sbi_hartindex_to_dom_context(__hartindex, __d) \
	(__d)->hartindex_to_context_table[__hartindex]

/** Macro to obtain the current hart's context pointer */
#define sbi_context_thishart_ptr()                         \
	sbi_hartindex_to_dom_context(                      \
		sbi_hartid_to_hartindex(current_hartid()), \
		sbi_domain_thishart_ptr())

/**
 * Enter a specific domain context synchronously
 * @param dom pointer to domain
 *
 * @return 0 on success and negative error code on failure
 */
int sbi_context_domain_enter(struct sbi_domain *dom);

/**
 * Exit the current domain context, and then return to the caller
 * of sbi_context_domain_enter or attempt to start the next domain
 * context to be initialized
 *
 * @return 0 on success and negative error code on failure
 */
int sbi_context_domain_exit(void);

/** Initialize contexts for all domains */
int sbi_context_mgmt_init(struct sbi_scratch *scratch);

#endif // __SBI_CONTEXT_H__
