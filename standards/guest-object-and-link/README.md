# Guest Object and Link

This proposal defines the other half of the [Static Library and Linker Script](../static-library-and-linker-script/README.md) standard. That standard says what a zkVM vendor ships. This one says what a guest team ships, the complete symbol interface between the two, and the link command that joins them into an ELF. It specifies:

- **The guest object**: a static archive of LLVM bitcode that defines `main` and leaves only the platform ABI undefined.
- **The platform ABI**: every symbol that may cross the boundary.
- **The SDK**: the vendor library, as fat LTO objects, and the linker script, with the rules that keep the link deterministic and accelerated code in effect.
- **The link**: `ld.lld` with full LTO across guest and SDK, so accelerator calls inline, and a native fallback for any other linker.

## Motivation

The static library standard lets a guest be compiled once, with a generic compiler, and linked against any vendor library. It leaves open what exactly the guest hands over, what else the two sides may assume of each other, and how the link runs. Each gap was hit in practice when the ethrex and reth stateless validators were linked against OpenVM, ZisK and SP1 libraries built from the vendors' own sources:

- **Termination.** A panic deep in a guest cannot return from `main`. The guest needs a function that ends the execution as a failure, and none is standardized.
- **The heap.** The static library standard gives `_heap_start`/`_heap_end` to the application, but every vendor library also allocates, from the same region. Two allocators starting at the same address overwrite each other.
- **Symbol collisions and silent fallback.** A vendor library built from Rust carries its own `core`, panic handler and allocator, which collide with the guest's. Accelerated `memcpy` in a separate archive member lost to the guest's weak `compiler_builtins` copy on OpenVM, with no diagnostic (see [Accelerated Memory Operations](../accelerated-memory-operations/README.md#linking-and-symbol-resolution)).
- **Performance across the boundary.** A per-call accelerator is often a few instructions, and the allocator is called on every allocation. Unless the linker optimizes guest and vendor code as one module, every call pays for an out-of-line call. Measured on the three zkVMs, one full-LTO module was 4–50% cheaper than separately optimized modules.
- **Reproducibility.** Two copies of the same compiler builtin, one on each side, made the output ELF, and so the verification key, depend on the order of the inputs.

## Goals

- Define the guest object so that it can be produced by any LLVM-based toolchain and linked against any conforming SDK without rebuilding.
- Define the complete set of symbols that may cross the guest/SDK boundary, with one owner for each.
- Make the link deterministic: the ELF is a function of the guest object, the SDK, the linker version and a fixed command.
- Keep accelerated code in effect, and inlinable, without making any toolchain other than LLVM a requirement for linking.

## Non-Goals

- Standardizing vendor internals, memory layout or the termination mechanism. Those stay vendor-defined behind the symbols below.
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
| `abort` | SDK | this proposal, [Termination](#termination) |
| `sys_alloc_aligned` | SDK | this proposal, [Heap](#heap) |
| compiler runtime builtins (`__muldi3`, `__udivti3`, …) | guest | the `libgcc`/`compiler-rt` ABI |

#### Termination

```c
_Noreturn void abort(void);
```

`abort` ends the execution as a failure, under the [Termination Semantics](../standard-termination-semantics/README.md) standard. A language runtime maps its abnormal terminations to it: a Rust panic handler, C `abort()` and a failed `assert()`. A normal end is `main` returning, as the static library standard defines.

#### Heap

```c
void* sys_alloc_aligned(size_t bytes, size_t align);
```

`sys_alloc_aligned` returns `bytes` bytes aligned to `align`, which is a power of two, from the zkVM's heap, and never returns memory it has returned before. Memory is never freed through this interface.

Until an SDK's library keeps its own memory outside `[_heap_start, _heap_end)`, a guest allocator must obtain its memory from `sys_alloc_aligned`, not from `_heap_start`/`_heap_end`, because the vendor library allocates from the same region. This is the one interim departure from the [Static Library and Linker Script](../static-library-and-linker-script/README.md) standard, under which the application owns that region and the vendor keeps its scratch memory in a region of its own. An SDK whose library does so says so, and a guest linked against it may then use `_heap_start`/`_heap_end` directly (see [Rationale](#why-the-heap-goes-through-the-sdk-for-now)).

#### Runtime hooks for `std`

Rust's `std` on `target_os = "zkvm"` calls further functions (`sys_panic`, `sys_write`, `sys_rand` and others, in `library/std/src/sys/pal/zkvm/abi.rs`). They are not part of the platform ABI. A guest that uses `std` supplies them itself, in terms of the symbols above: `sys_panic` calls `abort`, output to descriptors 1 and 2 is dropped or reported, and `sys_rand` is deterministic.

### Guest object

A guest object is a static archive (`ar` format) whose members are:

- LLVM bitcode modules, produced by an LLVM no newer than the linker's (see [Link](#link)).
- Native RISC-V objects that the language toolchain keeps out of LTO, such as compiler runtime builtins. Their global definitions must be weak.

The guest object must satisfy all of the following:

1. It defines `main` with the `int main(void)` ABI of the [Static Library and Linker Script](../static-library-and-linker-script/README.md) standard.
2. It defines no other global symbol that the SDK defines in the platform ABI. A guest that defines `zkvm_keccak256` or `sys_alloc_aligned` would silently replace the vendor's.
3. Every symbol it leaves undefined is in the platform ABI.
4. Its code targets the base [RISC-V target](../riscv-target/target.md), RV64IM, with the `lp64` ABI and the medium code model, and enables no further ISA extension. The extensions a zkVM supports are added at link time (see [SDK](#sdk)).

For Rust, the guest target is the stock `riscv64im-unknown-none-elf` specification with compare-and-swap enabled (`"atomic-cas": true`), which `alloc::sync` and most of the ecosystem need. A zkVM guest runs on one thread, so atomics are lowered to plain loads and stores before the bitcode is emitted (`-Cpasses=lower-atomic`), and the code needs no `A` extension. The guest package is a `staticlib` built with `lto = "fat"`, `codegen-units = 1`, `panic = "abort"` and `-Clinker-plugin-lto`, and `-Zbuild-std=core,alloc`, since the target is Tier 3. This emits one bitcode module, exporting only `main`, plus native `compiler_builtins`. A guest that uses `std` builds for the same ISA with `"os": "zkvm"` and `-Zbuild-std=std,panic_abort`, and supplies the [runtime hooks](#runtime-hooks-for-std). For C and C++, compile with `clang -flto` (full LTO, not ThinLTO) and archive with `llvm-ar`.

### SDK

An SDK is a directory holding the library and the linker script of the [Static Library and Linker Script](../static-library-and-linker-script/README.md) standard, and optionally two more files, with these additional requirements:

- **`libzkvm.a`** holds fat LTO objects: each object carries native RISC-V code and the same code as LLVM bitcode in a `.llvm.lto` section. The native code, built for RV64IM with the extensions of `zkvm.features` and the medium code model, serves any linker; the bitcode serves an LTO link. The library defines every SDK symbol of the platform ABI, and every other symbol is internal: the vendor's copies of its language runtime, its panic handler and its allocator must not be visible to the guest. No platform ABI definition is marked `noinline`. The library holds no compiler runtime builtins: the guest's toolchain supplies them, so a link holds one copy of each. A native object that is not a fat LTO object must be linked unconditionally (below).
- **`zkvm.ld`** names the library with `INPUT(-lzkvm)`, so that the link needs no vendor-specific arguments. It names every native object that must be linked unconditionally with `EXTERN(<symbol>)`, using a symbol only that object defines, because an archive member is otherwise only extracted to satisfy an undefined symbol, and a weak definition elsewhere satisfies it first.
- **`zkvm.ld`** declares its segments with `PHDRS`, so that the ELF and program headers are not placed in a loadable segment, and assigns every declared segment at least one section that is never empty: GNU ld emits a declared segment with no sections at address 0.
- The library's bitcode is produced by an LLVM no newer than the linker's.
- **`zkvm.features`** (optional) is one line of comma-separated LLVM target features, each prefixed with `+` (for example `+zbb,+unaligned-scalar-mem`). It lists the extensions beyond RV64IM that the zkVM executes and proves. Before an LTO link, they are appended to the `target-features` attribute of every function in the guest object's bitcode. A feature is listed only if the zkVM supports it for all guest code; `+unaligned-scalar-mem` in particular requires that misaligned loads and stores are proven, as `Zicclsm` in the [RISC-V target](../riscv-target/target.md) requires.
- **`zkvm-lto-plugin.so`** (optional) is an LLVM pass plugin, native code built against the linker's LLVM for the host that links. An LTO link loads it into the LTO pipeline, where it may apply code-generation transformations that only this zkVM supports. Its output must be deterministic and must not change what the program computes.

### Link

The guest ELF is produced by adding the SDK's `zkvm.features`, if any, to the guest object's bitcode functions, then running exactly this command, with the SDK directory `<sdk>`, the (rewritten) guest object `<guest.a>`, the LLVM options `<option>`, if any, and the SDK's plugin, if any:

```text
ld.lld -T <sdk>/zkvm.ld -L <sdk> --gc-sections --fat-lto-objects --lto-O3 [-mllvm <option>]... \
    [--load-pass-plugin=<sdk>/zkvm-lto-plugin.so] -o <guest.elf> <guest.a>
```

The LLVM options are this guest's tuning on this zkVM (for example `--inline-threshold`), and those a vendor's own toolchain applies to every guest (for example SP1's `-misched-prera-direction=bottomup`). Like the SDK, they are inputs of the link, and the ELF depends on them.

- `ld.lld` is an upstream LLVM release, and its LLVM major version is at least that of every bitcode producer. LLVM reads bitcode from older releases but not from newer ones.
- The order of the inputs is fixed as above.

A guest object without bitcode, or a linker without LTO, such as GNU ld, links the SDK's native code instead: `ld -T <sdk>/zkvm.ld -L <sdk> --gc-sections <objects> -lgcc`. That link is supported, and gives up only inlining across the boundary.

## Rationale

### Why the heap goes through the SDK, for now

The [Static Library and Linker Script](../static-library-and-linker-script/README.md) standard gives the application the heap, and its discussion rejected exporting an allocator: `malloc`/`free` force one language's allocator shape on the other, and the application should choose its allocation policy. That model needs the vendor library to keep its own memory apart, in a region of its own or a static array, reset between calls where nothing persists. Today's libraries do not: OpenVM and SP1 allocate from `_end` upward, and ZisK from the end of `.bss`, through the same `sys_alloc_aligned` Rust's `std` calls. ZisK's `ziskos` already has the separated mode (a static 8 MiB arena, reset per call, whose peak on 60 MiB blocks was just over 1 MiB), and the other vendors can follow. Until they do, one allocator through `sys_alloc_aligned` is the only safe choice, and it is what every guest running on these zkVMs does today, so nothing regresses. `sys_alloc_aligned` takes size and alignment and never frees, so it is not the `malloc` interface that was rejected, and the guest's allocator shape is unaffected when it moves to `_heap_start`/`_heap_end`.

### Why fat LTO objects

Only LLVM reads LLVM bitcode, and a bitcode-only library cannot be linked by GNU ld or by a toolchain without LTO. Native code is the one format every linker reads. A fat object carries both: an LTO link reads the bitcode and inlines across the boundary, and any other link uses the native code. Measured on reth over ten devnet blocks, an LTO link against fat objects retired the same instructions as against bitcode-only ones (to within 0.1%), and the native path 3.3–3.7% more on ZisK.

### Why bitcode, and why full LTO

The accelerator symbols are small: a hash call can be a single custom instruction. Measured on OpenVM, ZisK and SP1, a guest and SDK optimized as one full-LTO module retired 4–50% fewer instructions than the same code optimized separately, and the optimizer inlined the accelerators into their callers without `always_inline`. ThinLTO between the two sides inlined less, because a module without a summary cannot be imported from.

### Why compiler builtins belong to the guest

Every language toolchain supplies its own runtime builtins under the shared `libgcc`/`compiler-rt` names. With a second copy in the SDK, both copies are weak, and the order of the inputs decides which one is linked, which changes the verification key. With one copy, the order does not matter. Measured on reth, taking them from the guest cost 0.01% on ZisK.

### Why one internal module per SDK

A Rust vendor library built as a `staticlib` exports its panic handler (`rust_begin_unwind`) and carries its own `core`. Linked next to a Rust guest, the first failed the link on a duplicate symbol. Merging the library into one module and internalizing everything outside the platform ABI removes every such collision.

### Why ISA extensions are added at link time

A guest object compiled with an extension cannot run on a zkVM that lacks it: SP1's executor rejects the misaligned loads `+unaligned-scalar-mem` produces, while OpenVM and ZisK run faster with them, and ZisK also proves the bitmanip extensions. Code generation happens during the full-LTO link anyway, so the SDK can name the extensions and the link can apply them. They have to be written into each function's `target-features` attribute, because LLVM uses that attribute instead of, not in addition to, the target machine's features. Source code that selects a path with a compile-time feature test (Rust's `cfg(target_feature)`) cannot benefit, because that choice is made before the link.

### Why an SDK may carry an LTO plugin

Some zkVM acceleration lives in code generation, not in a function. ZisK proves a small fixed-size copy or comparison as one DMA operation when the code issues a specific instruction pattern, which ZisK's own compiler emits. Stock LLVM expands such copies into loads and stores during code generation, before any call to an accelerated `memcpy` exists, so neither the SDK library nor `zkvm.features` can reach them. A pass at the end of the full-LTO pipeline can. Without it, ethrex took about 10% more steps on devnet blocks. A zkVM that recognized stock LLVM's expansion would need no plugin, and would serve every toolchain.

### Why ABI functions must be inlinable

A vendor's own guest build compiles its runtime and the guest together, so its choices about inlining are made for that build. ZisK marks `sys_alloc_aligned` `#[inline(never)]` because its own `std` allocates without calling it. A generic guest calls it on every allocation: kept out of line, its 692 call sites cost reth 7,000 steps per block.

### Why `EXTERN` rather than link order

Relying on link order alone is not conforming under the [Accelerated Memory Operations](../accelerated-memory-operations/README.md) standard. `EXTERN` in the SDK's own linker script is mechanism (1) of that standard, an always-linked object, expressed without any flag in the guest's link command.

## Conformance

A conforming SDK passes these checks:

1. **ABI coverage.** `libzkvm.a` defines every SDK symbol of the platform ABI and exports nothing outside it, except the optional memory operations and symbols the vendor's own assembly references.
2. **Correctness.** A guest running the accelerator test vectors, linked against the SDK, completes successfully, and every vector's result matches, through both the LTO and the native link.
3. **Acceleration.** For each accelerator the vendor claims to accelerate, the same guest linked against the vendor's runtime with that `zkvm_*` symbol replaced by a plain RISC-V implementation retires measurably more instructions.

A conforming guest object passes the rules of [Guest object](#guest-object), which the link checks with `llvm-nm` before linking.

## Open questions

- Standardizing failed termination outside this proposal: every vendor already has the primitive, and a name such as `zkvm_abort` would not collide with a libc's `abort`.
- When SDK libraries keep their memory outside `[_heap_start, _heap_end)`, and how an SDK says so.
- The stock `riscv64im-unknown-none-elf` target lacks compare-and-swap and prebuilt `core`/`alloc`: an upstream Rust target for single-threaded zkVM guests would let guests build with a plain stable toolchain.
- Where libc lives for non-Rust guests: in the guest object, or in the SDK.
- Optional vendor extensions beyond this ABI, such as OpenVM's 256-bit integer hooks, which a guest could use only behind a capability check.
