Enhance Context Management for Domain
===================================================

The context management component in OpenSBI provides basic CPU
context initialization and management routines based on existing
domains in OpenSBI. The context management component was
originally designed to support a kind of security domain that could
yield CPU cores and be scheduled on demand, in which the UEFI
Secure Variable service runs.

Introduction
------------

Using the RISC-V PMP mechanism, the upper-layer software can be
restricted from accessing memory and IO resources, and an isolation
domain can be created. The OpenSBI Domain mechanism includes a
comprehensive definition and implementation of the isolation domain,
covering hardware resources, boot protocols, system privileges, and
more. As a fundamental feature of opensbi, each upper-layer software
operates within its respective domain.

The objective is to design and implement a context manager based on
domains. This context manager allows for efficient CPU switching
between different isolation domains, enabling the utilization of CPU
resources in scenarios where StMM, TEEOS, and other system runtime
services that require security isolation are enabled. It facilitates the
implementation of the least privilege and mutual isolation design.

Unlike other exclusive HARTs and long-running domains that determine
the initial context through the "next_*" field in the scratch structure and
save their context in CPU registers, dynamic domains like StMM rely on
the context manager to initialize and manage their context.

The reference architecture shown below illustrates the inclusion of
secure domains as runtime services, with the context manager
providing synchronous context switching when a service is called.

![image](https://github.com/Penglai-Enclave/opensbi/assets/55442231/7c01e611-a253-4dfd-8248-53fa2b3cce55)

Context Allocation and Initialization
-------------------------------------

The secure domain context is configured in the device tree (refer to
docs/domain_support.md, domain "context_mgmt_enabled" property).
After the domain configuration is completed and the SBI ecall interface
is ready, the cold-boot hart initializes and starts the secure domain.
The started secure domain is then connected to the context manager
through the SBI ecall interface. The context manager saves the context
and returns to the control flow before to continue with the next steps of
the OpenSBI startup process. The initialization process of the context
manager and security domain context is as follows:

![image](https://github.com/Penglai-Enclave/opensbi/assets/55442231/08d585af-4eea-4ca7-b071-2c2355b5fcbf")

Context Restore/enter and Save/exit
-------------------------------------

During system runtime, non-secure domains can hook a message to
the context manager through the ecall interface. The context manager
saves the current ecall handler context and switches to the security
domain for execution. Once the context within the secure domain has
been processed, it returns with the results to the prior ecall handler
context in M-mode, and then returns to the non-secure domain upon
completion of the ecall processing. The current sequence of context
saving and restoration in OpenSBI is as follows:

![image](https://github.com/Penglai-Enclave/opensbi/assets/55442231/8c40cf7f-7017-482c-8210-8f80f03254c1")

Conclusion
----------

The context switching process provided by the context management
component follows the requirements of domain isolation. It currently
does not handle the switching of external interrupt enable status.
