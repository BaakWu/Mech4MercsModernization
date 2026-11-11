#include "hook.h"

#include <cstring>

namespace mw4hook {
namespace {

bool EqualModuleName(const char* a, const char* b) {
    if (!a || !b) {
        return false;
    }
    return _stricmp(a, b) == 0;
}

bool IsReadableMemory(const void* address) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info)) {
        return false;
    }
    if (info.State != MEM_COMMIT) {
        return false;
    }
    if (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) {
        return false;
    }
    return true;
}

// Heuristic: a run of pointers that all land inside |target_module| is an IAT
// for that module, not arbitrary data in the image.
bool LooksLikeIat(void** slot, HMODULE target_module) {
    if (!target_module) {
        return false;
    }
    auto* base = reinterpret_cast<unsigned char*>(target_module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    const uintptr_t low = reinterpret_cast<uintptr_t>(base);
    const uintptr_t high = low + nt->OptionalHeader.SizeOfImage;

    int checked = 0;
    for (int i = 0; i < 32; ++i) {
        void* value = slot[i];
        if (!value) {
            break;
        }
        uintptr_t address = reinterpret_cast<uintptr_t>(value);
        if (address < low || address >= high) {
            return false;
        }
        if (++checked >= 4) {
            break;
        }
    }
    return checked > 0;
}

}  // namespace

// Replace one imported function pointer in a module's import address table by
// matching the already-resolved function address. This avoids depending on the
// packer's (nonstandard) thunk layout: it scans every plausible thunk array in
// the import descriptor for the DLL and patches slots that already point at
// |target|.
bool PatchIatByAddress(HMODULE module, const char* dll_name,
                       HMODULE target_module, void* target, void* detour,
                       void** original) {
    if (!module || !dll_name || !target_module || !target || !detour) {
        return false;
    }

    auto* base = reinterpret_cast<unsigned char*>(module);
    __try {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            return false;
        }
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) {
            return false;
        }
        const IMAGE_DATA_DIRECTORY& dir =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (dir.VirtualAddress == 0) {
            return false;
        }

        auto* desc =
            reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
        bool patched = false;
        for (; desc->Name != 0; ++desc) {
            const char* imported_dll = reinterpret_cast<const char*>(base + desc->Name);
            if (!EqualModuleName(imported_dll, dll_name)) {
                continue;
            }

            const uintptr_t base_addr = reinterpret_cast<uintptr_t>(base);
            const DWORD fields[2] = {desc->FirstThunk, desc->OriginalFirstThunk};
            for (DWORD field : fields) {
                if (!field) {
                    continue;
                }
                for (int mode = 0; mode < 2; ++mode) {
                    uintptr_t address = mode == 0 ? field : base_addr + field;
                    auto* slot = reinterpret_cast<void**>(address);
                    if (!IsReadableMemory(slot) || !LooksLikeIat(slot, target_module)) {
                        continue;
                    }
                    for (int i = 0; i < 1024; ++i) {
                        void* value = slot[i];
                        if (!value) {
                            break;
                        }
                        if (value != target) {
                            continue;
                        }

                        DWORD old_protect = 0;
                        if (!VirtualProtect(&slot[i], sizeof(void*),
                                            PAGE_READWRITE, &old_protect)) {
                            continue;
                        }
                        if (original && !*original) {
                            *original = value;
                        }
                        slot[i] = detour;
                        VirtualProtect(&slot[i], sizeof(void*), old_protect,
                                       &old_protect);
                        patched = true;
                    }
                }
            }
        }
        return patched;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace mw4hook
