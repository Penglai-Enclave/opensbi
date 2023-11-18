/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) IPADS@SJTU 2023. All rights reserved.
 */

#include <sbi/sbi_error.h>
#include <sbi/riscv_locks.h>
#include <sbi/riscv_asm.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_hsm.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_heap.h>
#include <sbi/sbi_scratch.h>
#include <sbi/sbi_string.h>
#include <sbi/sbi_domain_context.h>

/**
 * Switches the HART context from the current domain to the target domain,
 * and either restores or starts up the target domain context.
 * This includes changing domain assignments and reconfiguring PMP, as well
 * as saving and restoring CSRs and trap states.
 *
 * @param dom pointer to the target domain
 */
static void switch_to_next_domain_context(struct sbi_domain *dom)
{
	bool startup = false;
	u32 hartindex;
	struct sbi_trap_regs *trap_regs;
	struct sbi_context *ctx	    = sbi_domain_context_thishart_ptr();
	struct sbi_context *dom_ctx = sbi_hartindex_to_domain_context(
		sbi_hartid_to_hartindex(current_hartid()), dom);
	struct sbi_context startup_ctx = { 0 };
	struct sbi_scratch *scratch    = sbi_scratch_thishart_ptr();
	unsigned int pmp_count	       = sbi_hart_pmp_count(scratch);

	/*
	 * If target domain context not initialized, 
	 * mark startup, and use a clean context.
	 */
	if (!dom_ctx) {
		startup = true;
		dom_ctx = &startup_ctx;
	}

	/* Assign current hart to target domain */
	hartindex = sbi_hartid_to_hartindex(current_hartid());
	sbi_hartmask_clear_hartindex(
		hartindex, &sbi_domain_thishart_ptr()->assigned_harts);
	sbi_update_hartindex_to_domain(hartindex, dom);
	sbi_hartmask_set_hartindex(hartindex, &dom->assigned_harts);

	/* Reconfigure PMP settings for the new domain */
	for (int i = 0; i < pmp_count; i++) {
		pmp_disable(i);
	}
	sbi_hart_pmp_configure(scratch);

	/* Save current CSR context and restore target domain's CSR context */
	ctx->sstatus	= csr_swap(CSR_SSTATUS, dom_ctx->sstatus);
	ctx->sie	= csr_swap(CSR_SIE, dom_ctx->sie);
	ctx->stvec	= csr_swap(CSR_STVEC, dom_ctx->stvec);
	ctx->sscratch	= csr_swap(CSR_SSCRATCH, dom_ctx->sscratch);
	ctx->sepc	= csr_swap(CSR_SEPC, dom_ctx->sepc);
	ctx->scause	= csr_swap(CSR_SCAUSE, dom_ctx->scause);
	ctx->stval	= csr_swap(CSR_STVAL, dom_ctx->stval);
	ctx->sip	= csr_swap(CSR_SIP, dom_ctx->sip);
	ctx->satp	= csr_swap(CSR_SATP, dom_ctx->satp);
	ctx->scounteren = csr_swap(CSR_SCOUNTEREN, dom_ctx->scounteren);
	ctx->senvcfg	= csr_swap(CSR_SENVCFG, dom_ctx->senvcfg);

	/* Save current trap state and restore target domain's trap state */
	trap_regs = (struct sbi_trap_regs *)(csr_read(CSR_MSCRATCH) -
					     SBI_TRAP_REGS_SIZE);
	sbi_memcpy(&ctx->regs, trap_regs, sizeof(*trap_regs));
	sbi_memcpy(trap_regs, &dom_ctx->regs, sizeof(*trap_regs));

	/* If need, startup the boot HART of target domain */
	if (startup) {
		if (current_hartid() == dom->boot_hartid)
			sbi_hart_switch_mode(dom->boot_hartid, dom->next_arg1,
					     dom->next_addr, dom->next_mode,
					     false);
		else
			sbi_hsm_hart_stop(scratch, true);
	}
}

int sbi_domain_context_enter(struct sbi_domain *dom)
{
	struct sbi_context *ctx	    = sbi_domain_context_thishart_ptr();
	struct sbi_context *dom_ctx = sbi_hartindex_to_domain_context(
		sbi_hartid_to_hartindex(current_hartid()), dom);

	if (!ctx) {
		ctx = sbi_domain_context_thishart_ptr() =
			sbi_zalloc(sizeof(struct sbi_context));
		if (!ctx)
			return SBI_ENOMEM;
	}

	/* Validate the target domain context is initialized and runnable */
	if (!dom_ctx)
		return SBI_EINVAL;

	/* Update target context's previous context to indicate the caller */
	dom_ctx->prev_dom = sbi_domain_thishart_ptr();

	switch_to_next_domain_context(dom);

	return 0;
}

int sbi_domain_context_exit(void)
{
	u32 i, hartindex = sbi_hartid_to_hartindex(current_hartid());
	struct sbi_domain *dom;
	struct sbi_context *ctx = sbi_domain_context_thishart_ptr();

	if (!ctx) {
		ctx = sbi_domain_context_thishart_ptr() =
			sbi_zalloc(sizeof(struct sbi_context));
		if (!ctx)
			return SBI_ENOMEM;
	}

	dom = ctx->prev_dom;

	/* If no previous caller domain */
	if (!dom) {
		/* Find next uninitialized user-defined domain */
		sbi_domain_for_each(i, dom) {
			if (dom == &root || dom == sbi_domain_thishart_ptr())
				continue;

			if (sbi_hartmask_test_hartindex(hartindex,
							dom->possible_harts) &&
			    !sbi_hartindex_to_domain_context(hartindex, dom))
				break;
		}

		/* Take the root domain if fail to find */
		if (!dom)
			dom = &root;
	}

	switch_to_next_domain_context(dom);

	return 0;
}
