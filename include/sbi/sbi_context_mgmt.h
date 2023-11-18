/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) IPADS@SJTU 2023. All rights reserved.
 */

#ifndef __SBI_CONTEXT_H__
#define __SBI_CONTEXT_H__

#include <sbi/sbi_types.h>
#include <sbi/sbi_trap.h>

/** Representation of low level hart context */
struct sbi_context {
	/** General registers, mepc and mstatus for trap state */
	struct sbi_trap_regs regs;
	/** S-mode CSR registers */
	uint64_t csr_stvec;
	uint64_t csr_sscratch;
	uint64_t csr_sie;
	uint64_t csr_sip;
	uint64_t csr_satp;
	/**
	 * Stack address to restore M-mode C runtime context from after
	 * returning from a synchronous enter into this context.
	 */
	uintptr_t c_rt_ctx;
};

/**
 * Synchronous entry into specific domain context
 * @param domain_index the domain ID
 * @return return value carried when domain exits
 */
uint64_t sbi_context_domain_enter(u32 domain_index);

/**
 * Save current domain context and return to the place where
 * sbi_context_domain_enter() was called originally.
 * @param rc specified return value
 */
void sbi_context_domain_exit(uint64_t rc);

/** Initialize contexts of domains with context management enabled */
int sbi_context_mgmt_init(struct sbi_scratch *scratch);

#endif
