# YukiPLT integration



The Zygote and Native cores directly use the YukiPLT submodule's C++ API.

Each core contains a private static copy, built with matching NDK headers and

static libc++. Handles and C++ objects never cross core boundaries. The supported

injection topology selects one core per process; independently loaded copies

must not concurrently modify the same slots or pages.



## Ownership and recovery



Bootstrap, ART, native module bindings and HyperOS retain their own named image

identities, original storage and native handles. HyperOS installs its two slots

in one transaction. Zygisk registration queues belong to their invoking module;

commit prepares all groups first and runs one transaction per image (1–64 slots).

Different images can partially succeed. An incomplete operation blocks further

installation in that core until its owner completes recovery.



Unhook uses the original handle, preserves foreign slot values, and retains

failed recovery handles. ImageInvalidated consumes a handle without claiming a

restore write. Destructors do not unhook. Published original storage is never

cleared or reused, including after a rolled-back installation. A successful

unhook does not establish quiescence. Modules with published replacements remain

resident when no quiescence contract exists.



ZN API v4 and Zygisk table layouts are unchanged. Only the ZN pltHook boundary

preserves repeated requests for the same image/symbol/callback: it queries the

held handle and live slot before publishing the saved original to a checked new

output. This creates no new owner or hook chain. Competing replacements fail.

Use zn_plt_load_original or zygisk::loadPltOriginal to read module originals.



NamedImage does not pin an image or prove reload history. Callers keep target

images, replacement code and original storage alive through recovery and

quiescence. Only unique, publicly enumerable JUMP_SLOT targets are supported.

The frozen dependency contains the documented non-null host extension; ART

self-imports remain eligible. No upstream API or implementation is changed by

this host refactoring.



## Fork and residency



The host operation gate rejects contention, inherited state from an uncoordinated

PID change, and fork attempts observed with more than one thread. Controlled

single-threaded forks retain independent COW registry/handle state. No library

mutex, registry or TLS is reset. USAP specialization cannot itself adopt a new

PID; the preceding controlled fork must establish that handoff.



Thread counting does not prove that the actual fork is single-threaded: atfork

handlers or another replacement can create threads after the check. The library

still excludes calls in a multithreaded-fork child before exec. Actual Zygote and

HyperOS fork-boundary validation is an acceptance prerequisite; the host gate

and a smoke test do not waive that contract.



The current experiment includes the first two anonymous-lifecycle batches.
YukiPLT diagnostics use operation-owned stack state. Core-local unwind lookups
use only the Core's ELF metadata; ordinary discovery calls the real system
iterator. Exceptions must not cross Core boundaries.

Private pthread key creation and deletion use an allocation-free ledger sized
for PTHREAD_KEYS_MAX. Deletion consumes only successful registrations. The
current-thread drain repeats destructors that reinstall their value and leaves
keys registered. All other participating threads and their destructor epilogues
must have exited first; this helper does not establish quiescence.

The production drain/fini/self-unmap integration is not enabled. The existing
fork gate has a demonstrated atfork window, so the third batch is blocked pending
coordinated-fork evidence. The current system-loaded Core path is an incomplete
experiment, not the accepted anonymous-load/unload solution. This build has not
been deployed or accepted.

The final sequence must restore all hooks, consume recovery handles, release
owning objects and Registry storage, complete runtime-using finalizers, drain
thread storage, and then unregister the runtime key. Unregistration is
irreversible: compiler-rt does not reset its once/control indices. Only saved
scalar state and the existing C/assembly termination path may execute after it.
Actual unmapping, continued execution and thread exit need anonymous-path proof.

## Build and attribution



Use scripts/build.ps1 --clean for the complete signed Release, including all

supported LKMs and both native ABIs. Match deployed LKM, ksud, daemons and payload

hashes to the same build receipt; versionCode alone cannot identify an uncommitted

worktree. Host sanitizer and cross-build results do not establish OEM runtime

compatibility. Device operations require their own authorization.



YukiPLT is Apache-2.0. Imported copyright notices and LICENSE remain intact.

The gitlink at third_party/yukiplt and the Manager license inventory record the

exact source revision. Initialize it with `git submodule update --init --recursive`
before building. Other dependencies retain their own notices.
