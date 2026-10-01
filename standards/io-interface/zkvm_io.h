/**
 * zkVM IO C Interface
 *
 * This header defines the standard C interface for guest programs to access
 * private input, write public output and terminate as a failure.
 *
 * The functions follow:
 * https://github.com/eth-act/zkvm-standards/tree/main/standards/io-interface
 */

#ifndef ZKVM_IO_H
#define ZKVM_IO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Return the private input buffer.
 *
 * The returned pointer is read-only from the guest's perspective. The function
 * is idempotent and may be called multiple times.
 *
 * @param[out] buf_ptr Pointer receiving the input buffer address
 * @param[out] buf_size Pointer receiving the input buffer size in bytes
 */
void read_input(const uint8_t** buf_ptr, size_t* buf_size);

/**
 * Append bytes to the public output.
 *
 * Multiple calls are observed as if their byte buffers were concatenated.
 *
 * @param output Pointer to readable bytes
 * @param size Number of bytes to append
 */
void write_output(const uint8_t* output, size_t size);

#ifndef __cplusplus
/**
 * Terminate the execution as a failure.
 *
 * The execution halts and no valid proof of a successful execution can be
 * produced, as the Termination Semantics standard requires of abnormal
 * termination. It is the C library's `abort`: in C++, which `<cstdlib>`
 * declares as `std::abort`, it is not redeclared here.
 */
_Noreturn void abort(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ZKVM_IO_H */
