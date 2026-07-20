#ifndef NE_H
#define NE_H
#include "../types.h"
// Windows 3.x (NE) + 16-bit DOS (.COM) executable runner (#129).
// Loads the file, runs it on the x86_16 interpreter with DOS INT 21h and the
// Win16 API thunk (far calls into the thunk segment). Returns 0 on success.
int win16_run_file(const char *path);

// Resource access for the API layer (LoadBitmap/LoadString/LoadMenu/...).
// `hinst` selects the loaded module (the app or one of its DLLs); type/id are
// integer resource ids WITHOUT the 0x8000 flag. Returns a pointer into the
// retained module image and sets *out_len, or 0 if not found.
const uint8_t *win16_get_resource_first(uint16_t hinst, uint16_t type, uint32_t *out_len);
const uint8_t *win16_get_resource(uint16_t hinst, uint16_t type, uint16_t id,
                                  uint32_t *out_len);
const uint8_t *win16_get_resource_by_name(uint16_t hinst, uint16_t type,
                                          const char *name, uint32_t *out_len);

// Directory the launched app lives in (for resolving relative file opens).
const char *win16_get_appdir(void);

// Dynamic module export resolution (LoadLibrary / GetProcAddress, #148 Chips).
// win16_load_library returns a module handle for an already-loaded module (by
// basename) or a harmless non-error handle if not loaded. win16_get_proc_address
// resolves an export (by name, or by ordinal when name is empty) to a far pointer
// seg:off; returns 1 on success and 0 otherwise.
uint16_t win16_load_library(const char *name);
int win16_get_proc_address(uint16_t hmodule, const char *name, uint16_t ordinal,
                           uint16_t *out_seg, uint16_t *out_off);

// (#278 pass32) Resolve an exported ordinal of a loaded module BY NAME to a
// runtime far pointer seg:off. Unlike win16_get_proc_address this does not go
// through hinstance, so it works for a DATA-NONE DLL (autodata==0, e.g. SDM.DLL)
// whose hinstance is 0. Returns 1 on success and 0 otherwise.
int win16_module_export(const char *module, uint16_t ordinal,
                        uint16_t *out_seg, uint16_t *out_off);

#endif
