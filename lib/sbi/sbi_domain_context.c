/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) IPADS@SJTU 2023. All rights reserved.
 */

#include <sbi/sbi_error.h>
#include <sbi/riscv_asm.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_hsm.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_heap.h>
#include <sbi/sbi_scratch.h>
#include <sbi/sbi_string.h>
#include <sbi/sbi_domain_context.h>

/** 
 * Switches the hart context from the current domain to the target domain.
 * This includes changing domain assignments and reconfiguring PMP, as well
 * as saving and restoring CSRs and trap states.
 *
 * @param ctx pointer to the current hart context
 * @param dom_ctx pointer to the target domain context
 */
static void switch_to_next_domain_context(struct sbi_context *ctx,
					  struct sbi_context *dom_ctx)
{
	struct sbi_trap_regs *trap_regs;
	struct sbi_scratch *scratch = sbi_scratch_thishart_ptr();
	unsigned int pmp_count	    = sbi_hart_pmp_count(scratch);

	/* Assign the current hart to the domain of the target context */
	sbi_domain_assign_hart(dom_ctx->dom, current_hartid());

	/* Disable all PMP regions in preparation for re-configuration */
	for (int i = 0; i < pmp_count; i++) {
		pmp_disable(i);
	}
	/* Reconfigure PMP settings for the new domain */
	sbi_hart_pmp_configure(scratch);

	/* Save current CSR context and restore target domain's CSR context */
	ctx->sstatus	= csr_swap(CSR_SSTATUS, dom_ctx->sstatus);
	ctx->sie		= csr_swap(CSR_SIE, dom_ctx->sie);
	ctx->stvec		= csr_swap(CSR_STVEC, dom_ctx->stvec);
	ctx->sscratch	= csr_swap(CSR_SSCRATCH, dom_ctx->sscratch);
	ctx->sepc		= csr_swap(CSR_SEPC, dom_ctx->sepc);
	ctx->scause		= csr_swap(CSR_SCAUSE, dom_ctx->scause);
	ctx->stval		= csr_swap(CSR_STVAL, dom_ctx->stval);
	ctx->sip		= csr_swap(CSR_SIP, dom_ctx->sip);
	ctx->satp		= csr_swap(CSR_SATP, dom_ctx->satp);
	ctx->scounteren = csr_swap(CSR_SCOUNTEREN, dom_ctx->scounteren);
	ctx->senvcfg	= csr_swap(CSR_SENVCFG, dom_ctx->senvcfg);

	/* Save current trap state and restore target domain's trap state */
	trap_regs = (struct sbi_trap_regs *)(csr_read(CSR_MSCRATCH) -
					     SBI_TRAP_REGS_SIZE);
	sbi_memcpy(&ctx->regs, trap_regs, sizeof(*trap_regs));
	sbi_memcpy(trap_regs, &dom_ctx->regs, sizeof(*trap_regs));
}

int sbi_domain_context_enter(struct sbi_domain *dom)
{
	struct sbi_context *ctx	    = sbi_domain_context_thishart_ptr();
	struct sbi_context *dom_ctx = sbi_hartindex_to_domain_context(
		sbi_hartid_to_hartindex(current_hartid()), dom);

	/* Validate the domain context before entering */
	if (!dom_ctx || !dom_ctx->initialized)
		return SBI_EINVAL;

	/* Mark the current context initialized as it's about to be saved */
	ctx->initialized = true;

	switch_to_next_domain_context(ctx, dom_ctx);

	/* Update target domain context's next context to indicate the caller */
	dom_ctx->next_ctx = ctx;

	return 0;
}

/**
 * Starts up the next domain context by booting its boot hart. This
 * function verifies that all possible harts are properly assigned to the
 * domain prior to its startup, guaranteeing the correct initialization
 * of contexts. If the assignment is incomplete, the current hart will
 * be stoped to await.
 *
 * @param dom_ctx A pointer to the domain context which should be started.
 */
static void __noreturn startup_next_domain_context(struct sbi_context *dom_ctx)
{
	int rc;
	u32 i;
	struct sbi_domain *dom	    = dom_ctx->dom;
	struct sbi_scratch *scratch = sbi_scratch_thishart_ptr();

	/* Check possible harts assignment */
	sbi_hartmask_for_each_hartindex(i, dom->possible_harts) {
		/* If a hart is not assigned, stop the current hart */
		if (!sbi_hartmask_test_hartindex(i, &dom->assigned_harts))
			sbi_hsm_hart_stop(scratch, true);
	}

	/* If current hart is not the domain's boot hart, start boot hart */
	if (current_hartid() != dom->boot_hartid) {
		if ((rc = sbi_hsm_hart_start(scratch, dom, dom->boot_hartid,
					     dom->next_addr, dom->next_mode,
					     dom->next_arg1)))
			sbi_printf("%s: failed to start boot HART %d"
				   " for %s (error %d)\n",
				   __func__, dom->boot_hartid, dom->name, rc);
		/* Stop current hart which will be started by boot hart using hsm */
		sbi_hsm_hart_stop(scratch, true);
	}

	/* If current hart is the boot hart, jump to the domain first time */
	sbi_hart_switch_mode(dom->boot_hartid, dom->next_arg1, dom->next_addr,
			     dom->next_mode, false);
}

int sbi_domain_context_exit(void)
{
	struct sbi_context *ctx	    = sbi_domain_context_thishart_ptr();
	struct sbi_context *dom_ctx = ctx->next_ctx;

	if (!dom_ctx)
		return SBI_EINVAL;

	/* Mark the current context initialized as it's about to be saved */
	ctx->initialized = true;

	switch_to_next_domain_context(ctx, dom_ctx);

	/* If next context is initialized, no further action is needed */
	if (dom_ctx->initialized)
		return 0;

	/* Next context has not been initialized, start it up on current hart */
	startup_next_domain_context(dom_ctx);
	__builtin_unreachable();
}

/**
 * Allocates and configures context for all possible harts within a
 * given domain. Confirm the validity of boot hart and possible harts,
 * and construct the domain boot-up chain on each hart.
 *
 * @param hartindex_to_tail_ctx_table the tail context of boot-up chain
 * @param dom pointer to the domain being set up
 * @return 0 on success and negative error code on failure
 */
static int
setup_domain_context(struct sbi_context *hartindex_to_tail_ctx_table[],
		     struct sbi_domain *dom)
{
	int rc;
	u32 i;
	struct sbi_context *dom_ctx;

	/* Iterate over all possible harts and initialize their context */
	sbi_hartmask_for_each_hartindex(i, dom->possible_harts) {
		dom_ctx = sbi_zalloc(sizeof(struct sbi_context));
		if (!dom_ctx) {
			rc = SBI_ENOMEM;
			goto fail_free_all;
		}

		/* Initialize the domain context and add to domain's context table */
		dom_ctx->dom			   = dom;
		dom->hartindex_to_context_table[i] = dom_ctx;

		/* If assigned, it would be the head of boot-up chain */
		if (sbi_hartmask_test_hartindex(i, &dom->assigned_harts)) {
			hartindex_to_tail_ctx_table[i] = dom_ctx;
			continue;
		}

		/*
		 * If ROOT doamin, it would be the next context of tail context
		 * Note: The ROOT domain is the parameter for the last time
		 * function call, so the tail context must be present.
		 */
		if (dom == &root) {
			hartindex_to_tail_ctx_table[i]->next_ctx = dom_ctx;
			continue;
		}

		/*
		 * If not assigned, check that the domain configuration meets the
		 * criteria for context management, ensuring that each domain
		 * context is capable of proper initialization.
		 */
		if (sbi_hartmask_test_hartindex(
			    sbi_hartid_to_hartindex(dom->boot_hartid),
			    &dom->assigned_harts)) {
			sbi_printf(
				"%s: %s possible HART mask has unassigned hart %d at "
				"boot time, whose context can't be initialized\n",
				__func__, dom->name,
				sbi_hartindex_to_hartid(i));
			rc = SBI_EINVAL;
			goto fail_free_all;
		}

		if (!hartindex_to_tail_ctx_table[i]) {
			sbi_printf(
				"%s: %s possible HART mask has unassignable hart %d, "
				"domain contexts will never be started up\n",
				__func__, dom->name,
				sbi_hartindex_to_hartid(i));
			rc = SBI_EINVAL;
			goto fail_free_all;
		}

		/* If valid, append it to the boot-up chain */
		hartindex_to_tail_ctx_table[i]->next_ctx = dom_ctx;
		hartindex_to_tail_ctx_table[i]		 = dom_ctx;
	}

	return 0;

fail_free_all:
	/* Free any allocated context data in case of failure */
	sbi_hartmask_for_each_hartindex(i, dom->possible_harts)
		if (dom->hartindex_to_context_table[i])
			sbi_free(dom->hartindex_to_context_table[i]);
	return rc;
}

int sbi_domain_context_init(struct sbi_scratch *scratch)
{
	int rc;
	u32 i;
	struct sbi_domain *dom;

	/* Track tail context for context boot-up chain construction on harts */
	struct sbi_context
		*hartindex_to_tail_ctx_table[SBI_HARTMASK_MAX_BITS] = { 0 };

	/* Loop through each user-defined domain to configure its contexts */
	sbi_domain_for_each(i, dom) {
		if (dom != &root && (rc = setup_domain_context(
					     hartindex_to_tail_ctx_table, dom)))
			return rc;
	}

	/* Initialize ROOT domain contexts as default contexts */
	if ((rc = setup_domain_context(hartindex_to_tail_ctx_table, &root)))
		return rc;

	return 0;
}
