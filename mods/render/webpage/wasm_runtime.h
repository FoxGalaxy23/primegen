#ifndef MLB_WASM_RUNTIME_H
#define MLB_WASM_RUNTIME_H
#include <stddef.h>
#include <stdint.h>

typedef struct MlbWasmModule MlbWasmModule;
typedef struct MlbWasmInstance MlbWasmInstance;
typedef struct MlbWasmMemory MlbWasmMemory;
typedef struct MlbWasmGlobal MlbWasmGlobal;
typedef struct MlbWasmTable MlbWasmTable;
typedef struct MlbWasmValue { uint8_t type; uint64_t bits; } MlbWasmValue;
typedef int (*MlbWasmHostCall)(void *opaque, uint32_t import_index,
                               const MlbWasmValue *args, size_t argc,
                               MlbWasmValue *result, int *has_result,
                               char *error, size_t error_capacity);
typedef int (*MlbWasmHostCallMulti)(void *opaque, uint32_t import_index,
                               const MlbWasmValue *args, size_t argc,
                               MlbWasmValue *results, size_t results_capacity,
                               size_t *results_count, char *error, size_t error_capacity);

enum { MLB_WASM_I32=0x7f, MLB_WASM_I64=0x7e, MLB_WASM_F32=0x7d, MLB_WASM_F64=0x7c,
       MLB_WASM_EXTERNREF=0x6f, MLB_WASM_FUNCREF=0x70,
       MLB_WASM_FUNC=0, MLB_WASM_TABLE=1, MLB_WASM_MEMORY=2, MLB_WASM_GLOBAL=3 };

typedef struct MlbWasmImportValue {
    const char *module_name;
    const char *name;
    int kind;
    union { MlbWasmMemory *memory; MlbWasmGlobal *global; MlbWasmTable *table; void *opaque; } value;
} MlbWasmImportValue;

/* Returns NULL when the module is outside the currently implemented native instruction subset.
   The caller may use another standards-compliant engine as a compatibility fallback. */
MlbWasmModule *mlb_wasm_rt_module_new(const uint8_t *bytes, size_t length,
                                      char *error, size_t error_capacity);
void mlb_wasm_rt_module_free(MlbWasmModule *module);
size_t mlb_wasm_rt_export_count(const MlbWasmModule *module);
size_t mlb_wasm_rt_import_count(const MlbWasmModule *module);
int mlb_wasm_rt_import_info(const MlbWasmModule *module, size_t index,
                            const char **module_name, const char **name,
                            int *kind, uint32_t *function_index);
int mlb_wasm_rt_export_info(const MlbWasmModule *module, size_t index,
                            const char **name, int *kind, uint32_t *item_index);
int mlb_wasm_rt_find_export(const MlbWasmModule *module, const char *name,
                            int *kind, uint32_t *item_index);
int mlb_wasm_rt_func_signature(const MlbWasmModule *module, uint32_t function_index,
                               const uint8_t **params, size_t *param_count,
                               int *has_result, uint8_t *result_type);
int mlb_wasm_rt_func_signature_multi(const MlbWasmModule *module, uint32_t function_index,
                               const uint8_t **params, size_t *param_count,
                               const uint8_t **results, size_t *result_count);
/* Create a stable funcref value for the selected function in an instance.
   Function-reference values are opaque runtime tokens; zero represents null. */
int mlb_wasm_rt_function_ref_value(MlbWasmInstance *instance, uint32_t function_index,
                                   MlbWasmValue *out);
/* Bind an imported function index to its native source so funcref identity is
   preserved across module aliases. The target instance must outlive the alias. */
int mlb_wasm_rt_function_ref_alias(MlbWasmInstance *instance, uint32_t function_index,
                                   MlbWasmInstance *target, uint32_t target_function_index);
/* Resource accessors. Externref values are opaque 64-bit host handles; handle 0 is null. */
int mlb_wasm_rt_global_info(const MlbWasmModule *module, uint32_t global_index,
                            uint8_t *type, int *is_mutable);
int mlb_wasm_rt_global_get(const MlbWasmInstance *instance, uint32_t global_index,
                           MlbWasmValue *out);
int mlb_wasm_rt_global_set(MlbWasmInstance *instance, uint32_t global_index,
                           MlbWasmValue value);
int mlb_wasm_rt_table_size(const MlbWasmInstance *instance, uint32_t *size);
int mlb_wasm_rt_table_get(const MlbWasmInstance *instance, uint32_t index,
                          uint32_t *function_index);
int mlb_wasm_rt_table_set(MlbWasmInstance *instance, uint32_t index,
                          uint32_t function_index);
int64_t mlb_wasm_rt_table_grow(MlbWasmInstance *instance, uint32_t delta,
                              uint32_t initial_function_index);
int mlb_wasm_rt_table_get_ref(const MlbWasmInstance *instance, uint32_t index,
                              MlbWasmInstance **function_instance, uint32_t *function_index);
int mlb_wasm_rt_table_set_ref(MlbWasmInstance *instance, uint32_t index,
                              MlbWasmInstance *function_instance, uint32_t function_index);
MlbWasmInstance *mlb_wasm_rt_instance_new(MlbWasmModule *module,
                                         char *error, size_t error_capacity);
MlbWasmInstance *mlb_wasm_rt_instance_new_with_imports(MlbWasmModule *module,
                                         MlbWasmHostCall host_call, void *host_opaque,
                                         const MlbWasmImportValue *imports, size_t import_count,
                                         char *error, size_t error_capacity);
MlbWasmInstance *mlb_wasm_rt_instance_new_with_imports_multi(MlbWasmModule *module,
                                         MlbWasmHostCall host_call, MlbWasmHostCallMulti host_call_multi,
                                         void *host_opaque, const MlbWasmImportValue *imports,
                                         size_t import_count, char *error, size_t error_capacity);
MlbWasmInstance *mlb_wasm_rt_instance_new_with_host(MlbWasmModule *module,
                                         MlbWasmHostCall host_call, void *host_opaque,
                                         char *error, size_t error_capacity);
void mlb_wasm_rt_instance_free(MlbWasmInstance *instance);
int mlb_wasm_rt_call(MlbWasmInstance *instance, uint32_t function_index,
                     const MlbWasmValue *args, size_t argc,
                     MlbWasmValue *result, int *has_result,
                     char *error, size_t error_capacity);
int mlb_wasm_rt_call_multi(MlbWasmInstance *instance, uint32_t function_index,
                     const MlbWasmValue *args, size_t argc,
                     MlbWasmValue *results, size_t results_capacity, size_t *results_count,
                     char *error, size_t error_capacity);
uint8_t *mlb_wasm_rt_memory_data(MlbWasmInstance *instance, size_t *length,
                                uint32_t *pages, uint64_t *epoch);
int64_t mlb_wasm_rt_memory_grow(MlbWasmInstance *instance, uint32_t delta);
/* External MVP resources can be shared by independently instantiated modules. */
MlbWasmMemory *mlb_wasm_rt_memory_new(uint32_t initial_pages, int has_maximum,
                                      uint32_t maximum_pages, char *error,
                                      size_t error_capacity);
void mlb_wasm_rt_memory_retain(MlbWasmMemory *memory);
void mlb_wasm_rt_memory_release(MlbWasmMemory *memory);
MlbWasmMemory *mlb_wasm_rt_memory_handle(MlbWasmInstance *instance);
uint8_t *mlb_wasm_rt_memory_resource_data(MlbWasmMemory *memory, size_t *length,
                                          uint32_t *pages, uint64_t *epoch);
int64_t mlb_wasm_rt_memory_resource_grow(MlbWasmMemory *memory, uint32_t delta);
MlbWasmGlobal *mlb_wasm_rt_global_new(uint8_t type, int mutable_value,
                                      MlbWasmValue initial, char *error,
                                      size_t error_capacity);
void mlb_wasm_rt_global_retain(MlbWasmGlobal *global);
void mlb_wasm_rt_global_release(MlbWasmGlobal *global);
MlbWasmGlobal *mlb_wasm_rt_global_handle(MlbWasmInstance *instance,
                                         uint32_t global_index);
/* Shared MVP funcref tables. Table slots point at a function and its owning instance. */
MlbWasmTable *mlb_wasm_rt_table_new(uint32_t initial_size, int has_maximum,
                                    uint32_t maximum, char *error, size_t error_capacity);
MlbWasmTable *mlb_wasm_rt_table_new_typed(uint32_t initial_size, int has_maximum,
                                    uint32_t maximum, uint8_t element_type,
                                    char *error, size_t error_capacity);
int mlb_wasm_rt_table_element_type(const MlbWasmTable *table, uint8_t *element_type);
int mlb_wasm_rt_table_resource_get_externref(const MlbWasmTable *table, uint32_t index,
                                             uint64_t *value);
int mlb_wasm_rt_table_resource_set_externref(MlbWasmTable *table, uint32_t index,
                                             uint64_t value);
int64_t mlb_wasm_rt_table_resource_grow_externref(MlbWasmTable *table, uint32_t delta,
                                                  uint64_t initial_value);
/* True when an opaque externref handle is retained by a live native Global/Table. */
int mlb_wasm_rt_externref_is_live(uint64_t handle);
/* Enumerate externref values retained by an instance's globals and table. */
size_t mlb_wasm_rt_instance_externref_count(const MlbWasmInstance *instance);
int mlb_wasm_rt_instance_externref_at(const MlbWasmInstance *instance, size_t index, uint64_t *handle);
void mlb_wasm_rt_table_retain(MlbWasmTable *table);
void mlb_wasm_rt_table_release(MlbWasmTable *table);
MlbWasmTable *mlb_wasm_rt_table_handle(MlbWasmInstance *instance);
int mlb_wasm_rt_table_resource_size(const MlbWasmTable *table, uint32_t *size);
int mlb_wasm_rt_table_resource_get_ref(const MlbWasmTable *table, uint32_t index,
                                       MlbWasmInstance **function_instance, uint32_t *function_index);
int mlb_wasm_rt_table_resource_set_ref(MlbWasmTable *table, uint32_t index,
                                       MlbWasmInstance *function_instance, uint32_t function_index);
int64_t mlb_wasm_rt_table_resource_grow_ref(MlbWasmTable *table, uint32_t delta,
                                             MlbWasmInstance *function_instance,
                                             uint32_t function_index);
int mlb_wasm_rt_global_resource_get(const MlbWasmGlobal *global, MlbWasmValue *out);
int mlb_wasm_rt_global_resource_set(MlbWasmGlobal *global, MlbWasmValue value);
size_t mlb_wasm_rt_custom_section_count(const MlbWasmModule *module, const char *name);
int mlb_wasm_rt_custom_section_copy(const MlbWasmModule *module, const char *name,
                                    size_t index, uint8_t **bytes, size_t *length);
const char *mlb_wasm_rt_last_error(const MlbWasmInstance *instance);
#endif
