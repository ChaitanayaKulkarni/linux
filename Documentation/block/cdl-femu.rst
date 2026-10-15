.. SPDX-License-Identifier: GPL-2.0

Experimental FEMU CDL initial setup
===================================

This development series configures one validated CDL2FEMU PCI controller
at initial probe, before namespace publication. It assumes the controlled
FEMU successful-setup fixture. It provides no live layout replacement,
rollback transaction, scheduler restriction or CPU-hotplug policy. Reset
reconstruction and failure qualification are deferred. Existing kernel
checks, teardown and timeout handling remain enabled.

Configuration
-------------

Set the read-only parameters before loading the NVMe module, for example::

  modprobe nvme cdl_target=0000:00:04.0 \
    cdl_classes=1,2,3,4 cdl_durations_us=300,1000,2000,5000 \
    cdl_queues_per_class=4 cdl_ordinary_queues=4 cdl_queue_depth=64

For a built-in driver use the same names prefixed with ``nvme.`` on the
kernel command line. The PCI BDF must identify the actual FEMU controller.
Other controllers retain ordinary configuration. Parameters are copied
into controller-owned state and cannot be changed through sysfs. A new
configuration requires a fresh probe/boot.

Class IDs and duration arrays have equal lengths. Durations are unsigned
32-bit microseconds and use best-effort policy 02h. The initial dedicated
fixture requires one queue per possible CPU for ordinary I/O and each
class, with those CPUs initially online. Separate read/poll queue parameters
are not part of this profile. This specifies the initial fixture, not
runtime hotplug behavior. Scheduler compatibility remains TBD.

The driver validates the complete experimental identity and Identify limits,
requests both advertised Number of Queues maxima, and retains both grants.
It sets current ENABLE=0, configures current-only descriptors (SV=0), creates
all queues, constructs the final tag set/maps, verifies bindings/depth/count,
configures DBBUF, then enables CDL before namespace scan. Descriptor settings
are never replayed automatically during reset by this initial-only code.
A reset of the selected profile requires fresh probe/boot for renewed service.

Reference topology
------------------

The controller advertises seven class IDs including ordinary zero, at most
six bound classes, eight queues per class, 48 class SQs, and 52 I/O SQs/CQs.
The reference requests and receives 52/52, then creates only twenty pairs:
four ordinary plus four per selected class. The stable queue array has 21
entries, including admin. It uses 21 static unmanaged MSI-X vectors and seven
map slots; dedicated READ and POLL maps are empty. Unselected capacities do
not reserve extra host queues, maps or vectors for future live expansion.

Class ranges are ordered by duration at creation. With the example values::

  CPU  ordinary   C1        C2         C3         C4
   0      SQ1     SQ5       SQ9        SQ13       SQ17
   1      SQ2     SQ6       SQ10       SQ14       SQ18
   2      SQ3     SQ7       SQ11       SQ15       SQ19
   3      SQ4     SQ8       SQ12       SQ16       SQ20

Each SQ has its corresponding CQ. Physical admin Q0 is separate from the
presentation's logical class-queue labels Q0..Q15. All SQs in one class use
its shared duration/policy. Numeric class ID does not establish priority.
A 64-entry ring has 63 usable SQ/tag entries in this configuration.

Priority interfaces
-------------------

Block read/write uses existing kiocb/bio priority. Class selection uses the
complete ten-bit hint before taking a hardware tag. A hint naming a selected
class chooses its CPU map; that allocation CPU chooses its hctx/SQ. Task
migration after allocation retains the queue/tag ownership. Cache reuse,
merges and tag-wait remapping preserve compatible routes.

Legacy raw ioctls capture task priority before path/request allocation.
``NVME_IOCTL_IO64_CMD_PRIO`` and its vector form use the versioned 96-byte
priority container. Raw io_uring captures SQE priority once, requires driver
opt-in and validates nonzero namespace priority under request credentials.
Raw explicit zero stays ordinary. Block SQE zero retains normal task
inheritance. Unsupported/unconfigured raw nonzero command priority fails.
Admin requests stay on Q0. No Read/Write command priority field is invented;
Create SQ CDW11[10:3] carries the experimental class selector.

For example, BE class with hint 1 is ``(2 << 13) | (1 << 3)``. On allocation
CPU2 it selects hctx6/tag array6/SQ7/CQ7. Selection uses the effective priority
that reaches blk-mq. An effective NONE class, zero hint, unselected hint or
out-of-range hint uses an ordinary route without truncation.

Before allocation, normal block semantics can replace a NONE-class bio priority
with task priority. Block SQE zero captures task priority during preparation;
a nonzero NONE-class value can inherit at bio submission. Raw explicit zero
and NONE-class priorities reach the allocator directly and remain ordinary.
Thus a block SQE value and the final allocator priority are distinct inputs.

Polling retains precedence; P=0 means this fixture does not issue polled class
I/O. Internal Flush keeps its originating hctx/tag without becoming a timed
CDL command.

Observability and backend
-------------------------

Controller ``cdl_supported`` reports discovery; ``cdl_ready`` reports completed
initial setup. ``cdl_error`` reports discovery status. Probe logs report both
grants, selected counts and each class's duration/physical SQ range. Existing
NVMe setup/completion tracepoints include QID and CID; controller feature/log
readback supplies bindings and per-class accounting.

Use FEMU BBSSD ``femu_mode=1`` and a BBSSD namespace for the NAND path. WFQ
schedules admitted commands before to_ftl, then FTL/L2P/PPA and modeled NAND
service feed accounting and CQ completion. Initialize read ranges after boot
so PPAs exist and control caches; unmapped/cache-hit reads may avoid NAND.
Read, Write and DSM with Deallocate are timed; Flush and DSM without Deallocate
retain ordinary timer semantics. Late successful I/O remains successful.

NoSSD historical observations do not establish NAND performance. The FTL
worker's per-request modeled-service wait also needs parallelism review.
The series contains no selftest or fault-injection patches. Compile and
functional development checks do not qualify performance or the deferred
recovery, scheduler and hotplug behavior. Wire and ioctl allocations remain
local experimental interfaces, not ratified NVMe/Linux ABI assignments.
