#ifndef MLB_WASM_BINARY_H
#define MLB_WASM_BINARY_H
#include <stddef.h>
#include <stdint.h>

/*
 * Fast native structural preflight for WebAssembly binary envelopes.
 * This is deliberately not a full validator: instruction/type semantics remain
 * the embedded V8 runtime's responsibility until an in-process runtime lands.
 * Returns 1 when the header and section framing are well-formed, 0 otherwise.
 */
int mlb_wasm_binary_preflight(const uint8_t *bytes, size_t length,
                              char *error, size_t error_capacity);
#endif
