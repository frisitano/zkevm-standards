# Guest Object and Link

This proposal defines the other half of the [Static Library and Linker Script](../static-library-and-linker-script/README.md) standard. That standard says what a zkVM vendor ships. This one says what a guest team ships, the complete symbol interface between the two, and the one link command that joins them into an ELF. It specifies:

- **The guest object** — a static archive of LLVM bitcode that defines `main` and leaves only the platform ABI undefined.
- **The platform ABI** — every symbol that may cross the boundary, including termination, the heap, and the hooks a language runtime such as Rust's `std` needs.
- **The SDK** — the vendor library and linker script, with the extra rules that make the link deterministic and keep accelerated code from being silently dropped.
- **The link** — a pinned `ld.lld` with full LTO across guest and SDK, so accelerator calls can inline.

## Motivation

The static library standard lets a guest be compiled once, with a generic compiler, and linked against any vendor library. It leaves open what exactly the guest hands over, what else the two sides may assume of each other, and how the link runs. Each gap was hit in practice when the same guest (and the ethrex and reth stateless validators) was linked against OpenVM, ZisK and SP1 libraries built from the vendors' own sources:

- **Termination.** A Rust panic, a C `abort()` or a failed `assert()` has to end the execution through the vendor's termination mechanism, but no symbol for it is standardized. Without one, guests loop or trap in ways zkVMs do not agree on.
- **Language runtimes.** Rust's `std` on `target_os = "zkvm"` calls `sys_alloc_aligned`, `sys_write`, `sys_rand` and six other hooks. A vendor library that omits one fails the link; one that implements it differently changes behavior.
- **The heap.** The static library standard offers `_heap_start`/`_heap_end` to application allocators, but every vendor library also allocates internally. Two allocators starting at the same address overwrite each other.
- **Symbol collisions and silent fallback.** A vendor library built from Rust carries its own `core`, panic handler and allocator, which collide with the guest's. Accelerated `memcpy` in a separate archive member lost to the guest's weak `compiler_builtins` copy on OpenVM, with no diagnostic (see [Accelerated Memory Operations](../accelerated-memory-operations/README.md#linking-and-symbol-resolution)).
- **Performance across the boundary.** A per-call accelerator is often a few instructions. Unless the linker optimizes guest and vendor code as one module, every call pays for an out-of-line call and loses constant propagation. Measured on the three zkVMs, one full-LTO module was 4–50% cheaper than separately optimized modules.
- **Reproducibility.** Changing only the order of the two archives on the command line changed the output ELF, and so the verification key, on all three zkVMs.

## Goals

- Define the guest object so that it can be produced by any LLVM-based toolchain and linked against any conforming SDK without rebuilding.
- Define the complete set of symbols that may cross the guest/SDK boundary, with one owner for each.
- Make the link deterministic: the ELF is a function of the guest object, the SDK, the linker version and a fixed command.
- Keep accelerated code in effect, and make its absence detectable.

## Non-Goals

- Standardizing vendor internals, memory layout or the termination mechanism. Those stay vendor-defined behind the symbols below.
- Supporting guests without LLVM bitcode. A native-object guest can still be linked, but it gives up cross-boundary inlining, and this proposal does not treat that as a supported mode.
- Specifying a C library. Whether libc for non-Rust guests belongs in the guest object or the SDK is left open.

## Specification

### Platform ABI

The platform ABI is the complete set of symbols that may cross the boundary between a guest object and an SDK. A symbol not listed here must not cross it.

| Symbol | Defined by | Standard |
| --- | --- | --- |
| `_start` | SDK | [Static Library and Linker Script](../static-library-and-linker-script/README.md) |
| `main` | guest | [Static Library and Linker Script](../static-library-and-linker-script/README.md) |
| `read_input`, `write_output` | SDK | [IO Interface](../io-interface/README.md) |
| the `zkvm_*` accelerators | SDK | [Cryptographic Accelerators C Interface](../c-interface-accelerators/README.md) |
| `memcpy`, `memmove`, `memset`, `memcmp` (optional) | SDK | [Accelerated Memory Operations](../accelerated-memory-operations/README.md) |
| `abort`, `exit` | SDK | this proposal |
| `sys_alloc_aligned`, `sys_alloc_words` | SDK | this proposal, [Heap](#heap) |
| `sys_panic`, `sys_write`, `sys_read`, `sys_rand`, `sys_getenv`, `sys_argc`, `sys_argv` | SDK | this proposal, [Runtime hooks](#runtime-hooks) |

#### Termination

```c
_Noreturn void abort(void);
_Noreturn void exit(int status);
```

`abort` terminates the execution as a failure. `exit` terminates it with `status` exactly as if `main` had returned `status`, under the [Termination Semantics](../standard-termination-semantics/README.md) standard: zero is success, and non-zero is failure. A language runtime maps its abnormal terminations (a Rust panic without recovery, C `abort()` and failed `assert()`) to `abort`, or to `exit` with a non-zero status.

#### Heap

```c
void* sys_alloc_aligned(size_t bytes, size_t align);
uint32_t* sys_alloc_words(size_t nwords);
```

The SDK owns the heap. `sys_alloc_aligned` returns `bytes` bytes aligned to `align`, which is a power of two, and never returns memory it has returned before. `sys_alloc_words(n)` is `sys_alloc_aligned(4 * n, 4)`. Memory is never freed through this interface. A guest allocator must obtain its memory from `sys_alloc_aligned`. It must not take memory from `_heap_start`, `_heap_end`, `_end` or any other linker symbol, because the vendor library allocates from the same region.

This amends the [Static Library and Linker Script](../static-library-and-linker-script/README.md) standard. `_heap_start` and `_heap_end` remain defined, but they describe the region the SDK allocates from; they do not invite a second allocator.

#### Runtime hooks

These are the functions Rust's `std` calls on `target_os = "zkvm"` (`library/std/src/sys/pal/zkvm/abi.rs`). An SDK must define all of them, so that a guest can use `std` without knowing which zkVM it runs on.

```c
_Noreturn void sys_panic(const uint8_t* msg, size_t len);
void sys_write(uint32_t fd, const uint8_t* buf, size_t len);
size_t sys_read(uint32_t fd, uint8_t* buf, size_t len);
void sys_rand(uint32_t* buf, size_t words);
size_t sys_getenv(uint32_t* out_words, size_t out_nwords, const uint8_t* name, size_t name_len);
size_t sys_argc(void);
size_t sys_argv(uint32_t* out_words, size_t out_nwords, size_t index);
```

- `sys_panic` terminates as a failure, like `abort`, after optionally reporting `msg` to the host.
- `sys_write` to descriptor 1 or 2 may report the bytes to the host, or discard them. It must not change the public output, which only `write_output` writes.
- `sys_read` returns 0: there is no standard input.
- `sys_rand` fills `words` 32-bit words. The values are vendor-defined and are not a source of secret randomness. A guest that must be deterministic must not call it.
- `sys_getenv` returns `SIZE_MAX` (no such variable). `sys_argc` returns 0, and `sys_argv` returns 0.

### Guest object

A guest object is a static archive (`ar` format) whose members are:

- LLVM bitcode modules, produced by an LLVM no newer than the linker's (see [Link](#link)). Bitcode is required, because the link optimizes guest and SDK code as one module.
- Optionally, native RISC-V objects that the language toolchain keeps out of LTO, such as compiler runtime builtins. Their global definitions must be weak.

The guest object must satisfy all of the following:

1. It defines `main` with the `int main(void)` ABI of the [Static Library and Linker Script](../static-library-and-linker-script/README.md) standard.
2. It defines no other global symbol that is in the platform ABI. A guest that defines `zkvm_keccak256` or `sys_alloc_aligned` would silently replace the vendor's.
3. Every symbol it leaves undefined is in the platform ABI.
4. Its code targets the base [RISC-V target](../riscv-target/target.md), RV64IM, with the `lp64` ABI and the medium code model, and enables no further ISA extension. The extensions a zkVM supports are added at link time (see [SDK](#sdk)).

For Rust, the generic target is the stock `riscv64ima` bare-metal specification with `"os": "zkvm"`, so that `std` builds on the upstream zkVM platform port. The guest package is a `staticlib` built with `lto = "fat"`, `codegen-units = 1`, `panic = "abort"` and `-Clinker-plugin-lto`. This emits one bitcode module, exporting only `main`, plus native `compiler_builtins`. For C and C++, compile with `clang -flto` (full LTO, not ThinLTO) and archive with `llvm-ar`.

### SDK

An SDK is a directory holding the library and the linker script of the [Static Library and Linker Script](../static-library-and-linker-script/README.md) standard, and optionally a list of ISA extensions, with these additional requirements:

- **`libzkvm.a`** holds one LLVM bitcode module that defines every SDK symbol of the platform ABI. Every other symbol of that module is internal: the vendor's copies of its language runtime, its panic handler and its allocator must not be visible to the guest. The archive may also hold native objects. A native object other than compiler runtime builtins must be linked unconditionally (below).
- **`zkvm.ld`** names the library with `INPUT(-lzkvm)`, so that the link needs no vendor-specific arguments. It names every native object that must be linked unconditionally with `EXTERN(<symbol>)`, using a symbol only that object defines, because an archive member is otherwise only extracted to satisfy an undefined symbol, and a weak definition elsewhere satisfies it first.
- **`zkvm.ld`** declares its segments with `PHDRS`, so that the ELF and program headers are not placed in a loadable segment. Without it, `ld.lld` loads them in a segment just below the first section, which can fall inside the stack.
- The library's bitcode is produced by an LLVM no newer than the linker's.
- **`zkvm-lto-plugin.so`** (optional) is an LLVM pass plugin, native code built against the linker's LLVM for the host that links. The link loads it into the LTO pipeline, where it may apply code-generation transformations that only this zkVM supports. Its output must be deterministic and must not change what the program computes.
- **`zkvm.features`** (optional) is one line of comma-separated LLVM target features, each prefixed with `+` (for example `+zbb,+unaligned-scalar-mem`). It lists the extensions beyond RV64IM that the zkVM executes and proves. Before linking, they are appended to the `target-features` attribute of every function in the guest object's bitcode. A feature is listed only if the zkVM supports it for all guest code; `+unaligned-scalar-mem` in particular requires that misaligned loads and stores are proven, as `Zicclsm` in the [RISC-V target](../riscv-target/target.md) requires.

### Link

The guest ELF is produced by adding the SDK's `zkvm.features`, if any, to the guest object's bitcode functions, then running exactly this command, with the SDK directory `<sdk>`, the (rewritten) guest object `<guest.a>`, the guest's LLVM options `<option>` for this zkVM, if any, and the SDK's plugin, if any:

```text
ld.lld -T <sdk>/zkvm.ld -L <sdk> --gc-sections --lto-O3 [-mllvm <option>]... \
    [--load-pass-plugin=<sdk>/zkvm-lto-plugin.so] -o <guest.elf> <guest.a>
```

The LLVM options are the guest team's tuning for one zkVM (for example `--inline-threshold`). Like the SDK, they are inputs of the link, and the ELF depends on them.

- `ld.lld` is an upstream LLVM release, and its LLVM major version is at least that of every bitcode producer. LLVM reads bitcode from older releases but not from newer ones. The tooling therefore uses the newest validated LLVM, and producers may lag behind it.
- The order of the inputs is fixed as above. `ld.lld` resolves archives independently of their order, but where both sides carry a weak definition of the same symbol (typically compiler runtime builtins), the order decides which copy is used. That changes the ELF, and so the verification key, even when the copies are equivalent.
- Linking without LTO across the boundary is not a supported mode.

## Rationale

### Why the SDK owns the heap

Every vendor library allocates: IO buffers, precompile scratch space, the runtime's own `Vec`s. Either the vendor library allocates through a guest-supplied hook, or the guest allocates through a vendor-supplied one. The second option needs no new symbol, because Rust's `std` on `target_os = "zkvm"` already calls `sys_alloc_aligned`, and it keeps the heap policy (bump, linked list, reserved input region) with the party that knows the memory layout. OpenVM, ZisK and SP1 each already route both their own allocations and `sys_alloc_aligned` to a single heap.

### Why bitcode, and why full LTO

The accelerator symbols are small: a hash call can be a single custom instruction. Measured on OpenVM, ZisK and SP1, a guest and SDK optimized as one full-LTO module retired 4–50% fewer instructions than the same code optimized separately, and the optimizer inlined the accelerators into their callers without `always_inline`. ThinLTO between the two sides inlined less, because a module without a summary cannot be imported from.

### Why one internal module per SDK

A Rust vendor library built as a `staticlib` exports its panic handler (`rust_begin_unwind`) and carries its own `core`. Linked next to a Rust guest, the first failed the link on a duplicate symbol. Merging the library into one module and internalizing everything outside the platform ABI removes every such collision while keeping the library in bitcode.

### Why ISA extensions are added at link time

A guest object compiled with an extension cannot run on a zkVM that lacks it: SP1's executor rejects the misaligned loads `+unaligned-scalar-mem` produces, while OpenVM and ZisK run faster with them, and ZisK also proves the bitmanip extensions. Building one object per zkVM would give up the single guest object. Code generation happens during the full-LTO link anyway, so the SDK can name the extensions and the link can apply them. They have to be written into each function's `target-features` attribute, because LLVM uses that attribute instead of, not in addition to, the target machine's features. Applied at link time, they gave the same code as compiling them in: 1,036,035 against 1,036,094 instructions for an ethrex block on OpenVM. Source code that selects a path with a compile-time feature test (Rust's `cfg(target_feature)`) cannot benefit, because that choice is made before the link.

### Why an SDK may carry an LTO plugin

Some zkVM acceleration lives in code generation, not in a function. ZisK proves a small fixed-size copy or comparison as one DMA operation when the code issues a specific two-instruction pattern, which ZisK's own compiler emits for such copies. Stock LLVM expands them into loads and stores during code generation, before any call to an accelerated `memcpy` exists, so neither the SDK library nor `zkvm.features` can reach them. A pass at the end of the full-LTO pipeline can: the optimizer has already removed every copy it could, and code generation follows. On one ethrex block the ZisK plugin issues 9,781 such operations and saves 58,757 of 714,089 steps.

### Why guest tuning options belong to the link

Optimizer tuning is specific to a guest on a zkVM: ere builds ethrex for ZisK with 23 LLVM options tuned by search, and no other pair. Inlining and loop optimization happen during the full-LTO link, so the options take effect there, from one zkVM-agnostic guest object. For ethrex on ZisK they reduced one block from 655,332 to 510,301 steps.

### Why `EXTERN` rather than link order

Relying on link order alone is not conforming under the [Accelerated Memory Operations](../accelerated-memory-operations/README.md) standard. `EXTERN` in the SDK's own linker script is mechanism (1) of that standard, an always-linked object, expressed without any flag in the guest's link command.

## Conformance

A conforming SDK passes these checks, which a shared toolchain runs before admitting it:

1. **ABI coverage.** `libzkvm.a` defines every SDK symbol of the platform ABI and exports nothing outside it, except optional memory operations and linker-script symbols.
2. **Correctness.** A guest running the accelerator test vectors, linked against the SDK, completes successfully, and every vector's result matches.
3. **Acceleration.** For each accelerator the vendor claims to accelerate, the same guest linked against a control SDK retires measurably more instructions. The control SDK is the vendor's runtime with every `zkvm_*` symbol replaced by a plain RISC-V implementation.

A conforming guest object passes the three rules of [Guest object](#guest-object), which the toolchain checks with `llvm-nm` before linking.

## Open questions

- Where libc lives for non-Rust guests: in the guest object, or in the SDK.
- Whether guests built with GCC-based toolchains must switch to clang, since bitcode is required.
- Optional extensions beyond this ABI, for example vendor-specific hint interfaces. They could be standardized as optional symbols that a guest may only use behind a capability check.
