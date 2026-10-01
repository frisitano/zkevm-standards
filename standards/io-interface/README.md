This proposal aims to standardize zkVM IO interface. The input is the private input, the existential part of the relation being proven. The output is the public part of that relation.

# Motivation
Application developers need a zkVM independent way to access input and output. Then the code is portable and can be compiled without changes for various zkVMs.

# Goals
The goal is to define a portable, efficient and convenient way to do IO in zkVMs.

# The interface

The interface consists of the following C function prototypes:
- `void read_input(const uint8_t** buf_ptr, size_t* buf_size)`
- `void write_output(const uint8_t* output, size_t size)`
- `_Noreturn void abort(void)`

The `read_input` function is used as follows:

```
    const uint8_t* buf_ptr;
    size_t buf_size;
    read_input(&buf_ptr, &buf_size);
```

After calling `read_input`, the `buf_ptr` variable contains a pointer to the input, and `buf_size` indicates its size. The data provided by `read_input` is the private input described in the introduction. If `buf_size` is 0, then `buf_ptr` may contain an arbitrary value and should be considered invalid. The `read_input` function cannot fail, so no error code is returned. The type `const uint8_t*` for `buf_ptr` indicates that the memory area containing the input may be read-only and that the application code must not write to the memory pointed to by `buf_ptr`. This function has no side effects beyond setting the pointer parameters, may be called multiple times, and is idempotent.

zkVMs that don't preload input will need to read the entire input into an internal buffer during machine initialization to ensure `read_input` can be safely called from `main`. As a consequence, the size of the machine input must be smaller than the zkVM's addressable space.

`write_output` may be called multiple times. The observable result of calling `write_output` multiple times is as if the buffers from successive calls were concatenated, forming the entirety of the public part described in the introduction. Theoretically, this allows writing an unbounded amount of data across multiple calls, more than the maximum value representable by `size_t`, although this is not a practical concern. This function cannot fail, so no error code is returned. The pointer passed to `write_output` must point to readable memory; which memory segments are readable is zkVM-dependent. The 0 (NULL) pointer is not treated specially by this specification.

`abort` terminates the execution as a failure, under the [Termination Semantics](../standard-termination-semantics/README.md) standard: the execution halts, and no valid proof of a successful execution can be produced. It is the C library's `abort`, so a C program's `abort()` and failed `assert()` reach it directly, and a language runtime maps its other abnormal terminations to it, such as a Rust panic without recovery. A `no_std` Rust guest's panic handler calls it. The header declares it for C only: C++ declares the same function as `std::abort` in `<cstdlib>`, with an exception specification a second declaration could contradict.

# Rationale

`read_input` enables zero-copy implementations for zkVMs that preload input into memory, which justifies the departure from the standard libc IO interface. This approach naturally precludes interactive input reading, though this limitation is not a concern for EF's use case.

The `read_input` and `write_output` functions are independent of the libc IO interface. This proposal neither requires the presence of libc IO functions nor prescribes their behavior.