Enhance Context Management for Domain
===================================================

The domain context management component in OpenSBI provides basic CPU
context management routines for existing OpenSBI domain. The context
management component was initially designed to facilitate the suspension
and resumption of domains, enabling secure domains to efficiently
share CPU resources, and allowing the UEFI Secure Variable service to
run within it.

Introduction
------------

Using the RISC-V PMP mechanism, the upper-layer software can be
restricted from accessing memory and IO resources, and an isolation
domain can be created. The OpenSBI Domain mechanism includes a
comprehensive definition and implementation of the isolation domain,
covering hardware resources, boot protocols, system privileges, and
more. As a fundamental feature of OpenSBI, each upper-layer software
operates within its respective domain.

The objective is to design and implement a domain hart context
manager. This context manager allows for efficient CPU switching
between different isolation domains, enabling the utilization of CPU
resources in scenarios where StMM, TEEOS, and other system runtime
services that require security isolation are enabled. For example,
a system service domain might proactively suspend its execution after
initialization, yielding CPU resources, and then be scheduled to
execute on-demand when other domains request its services. It makes
domains suitable for running system services that follow the principles
of least privilege and mutual isolation.

The context management is based on the entity **struct sbi_context**, which
allows for suspension or resumption of execution for each hart within a
domain. At runtime, the context manager offers two interfaces:
`sbi_domain_context_enter` and `sbi_domain_context_exit`, both of which
perform domain context switching on the current hart. The crucial difference is
that the latter does not specify a target domain when called and simply
signifies that the caller wishes to exit and suspend the current context.
The context into which the hart will enter is determined by the context
manager, which may be the caller who entered the current context via
`sbi_domain_context_enter`, or it may be the next domain context awaiting
startup.

The reference architecture shown below illustrates the scenario of employing
enhanced secure domains to support runtime services, with the context manager
providing synchronous context switching when a service is called.

![image](https://github.com/Penglai-Enclave/opensbi/assets/55442231/a3fa8fff-8ec9-4ad6-b748-c2ee724a3060)

Context Structure
---------------

The **sbi_context** structure holds the state of a hart within a domain:

```
/** Context representation for a hart within a domain */
struct sbi_context {
	/** Trap-related states such as GPRs, mepc, and mstatus */
	struct sbi_trap_regs regs;

	/** Supervisor Status Register */
	unsigned long sstatus;
	/** Supervisor interrupt enable register */
	unsigned long sie;
	/** Supervisor trap vector base address register */
	unsigned long stvec;
	/** Supervisor scratch register for temporary storage */
	unsigned long sscratch;
	/** Supervisor exception program counter register */
	unsigned long sepc;
	/** Supervisor cause register */
	unsigned long scause;
	/** Supervisor trap value register */
	unsigned long stval;
	/** Supervisor interrupt pending register */
	unsigned long sip;
	/** Supervisor address translation and protection register */
	unsigned long satp;
	/** Counter-enable Register */
	unsigned long scounteren;
	/** Supervisor environment configuration register */
	unsigned long senvcfg;

	/** Reference to the owning domain */
	struct sbi_domain *dom;
	/** Next context to jump to during context exits */
	struct sbi_context *next_ctx;
	/** Current state of the context (idle or reset) */
	int state;
};
```

Relationship Between Domain and Context
-------------------------------------

In the current design, harts are capable of switching between domains. In
this setup, **"possible harts" corresponds to all harts contained within a**
**domain, with each hart being associated with a context**, indicating that
a hart is not necessarily required to be running in a domain at all times,
supporting context saving during exit and restoration during entry. On the
other hand, "assign harts" from 'opensbi-domain' property in the cpu node in
DT configuration determines the initial assignment of harts at platform
startup.

Recall that previously, harts could not be switched, and the set of harts
contained within a domain was, in fact, the initially computed "assigned
harts," i.e., the intersection of "possible harts" and "assign harts." The
domain would start and run long-term on the assigned harts, while any other
possible harts that were filtered out because they did not belong to assign
harts were simply discarded. Now, harts can switch between domains, and the
possible harts of a domain(let's assume domain A), could exit from another
domain and switch to domain A. As part of domain A, this hart cannot be
discarded, and its context needs to be initialized correctly.

To ensure that all the contexts of a domain are correctly initialized and
started, current logic dictates that the startup process of a domain will
only commence when all possible harts of a domain are assigned to that
domain (become assigned harts).

Functionalities and Interfaces
-------------------------------------

### Context Allocation and Configuration

`sbi_domain_context_init` is the initialization function called during the
OpenSBI boot process. It sets up the contexts for each domain present in the
system by calling `setup_domain_context`, which is the helper function that
allocates the contexts for possible harts of the specified domain. It
maintains a `hartindex_to_tail_ctx_table` to record the last discovered
context on each hart and, based on this, configures the next_ctx field of
the context, constructing a context boot-up chain on each hart. During
construction, it performs configuration checks to ensure that the user's
settings are consistent with their expected behavior.

The current sequence of context allocation and configuration is as follows:

![image](https://github.com/Penglai-Enclave/opensbi/assets/55442231/28fd4089-dec7-43c3-a1e8-cd6a74152b79)

In order to provide a clearer explanation, consider the following device
tree configuration. After completing the context setup, the relationship
between each context is as shown in the following diagram. The "next_ctx"
field of each context stores the next context to be initialized in the same
hart, forming a boot-up chain. Upon platform startup, all harts are
allocated to the StMM domain, and StMM starts.

```
    chosen {
        opensbi-domains {
            ...
            stmm: stmm-domain {
                compatible = "opensbi,domain,instance";
                possible-harts = <&cpu0 &cpu1 &cpu2 &cpu3>;
                ...
            };

            udomain: untrusted-domain {
                compatible = "opensbi,domain,instance";
                possible-harts = <&cpu1 &cpu2 &cpu3>;
                boot-hart = <&cpu1>;
                ...
            };

            tdomain: trusted-domain {
                compatible = "opensbi,domain,instance";
                possible-harts = <&cpu0>;
                boot-hart = <&cpu0>;
                ...
            };
        };
    };

    cpus {
        ...
        cpu0: cpu@0 {
            ...
            opensbi-domain = <&tdomain>;
        };

        cpu1: cpu@1 {
            ...
            opensbi-domain = <&udomain>;
        };

        cpu2: cpu@2 {
            ...
            opensbi-domain = <&udomain>;
        };

        cpu3: cpu@3 {
            ...
            opensbi-domain = <&udomain>;
        };
    };
```

![image](https://github.com/Penglai-Enclave/opensbi/assets/55442231/13433f1f-695e-4575-b329-06801c71a40b)

### Context Enter and Exit

During system runtime, domains can hook a message to the context manager
through the ecall interface. There are two types of messages: "enter" and
"exit".

The enter interface (`sbi_domain_context_enter`) transitions a hart into a
specified target domain's context synchronously if it's available. The
current hart's context is saved, and the target domain's context is loaded
and executed. This is encapsulated within the helper function
`switch_to_next_domain_context`.

To maintain the relationship between the caller and the callee, allowing the
callee domain to return to the previous caller domain upon exiting, the
function sets the "next_ctx" field of the target domain context to indicate
the caller. Please note that because the callee's context state is
available, it must have exited previously. Its "next_ctx" field,
representing the next context to be started, has already taken effect and
can be overridden.

The current sequence of context saving and restoration in OpenSBI is as
follows:

![image](https://github.com/Penglai-Enclave/opensbi/assets/55442231/887d26f5-c08f-45bb-b89c-cc344a0fde0b)

The exit interface (`sbi_domain_context_exit`) is used when a hart needs to
exit its current domain context. This function is typically called when a
hart needs to yield control to other domains.

When the "next_ctx" needs to be initialized—such as when the domain is first
entered—additional steps are taken to start up the next domain: start the
domain boot hart or halt the current hart if domain's possible harts not yet
fully assigned.

Test Domain Payload and Configuration
----------

We have developed a sample application demonstrating secure/non-secure
domain switching on a dual-core Qemu platform, providing device tree
configuration and the test payload source code. Please refer to
https://github.com/Shang-QY/test_context_switch/blob/master/README.md

Conclusion
----------

The context switching process provided by the context management
component follows the requirements of domain isolation. It currently
does not handle the switching of external interrupt enable status.
