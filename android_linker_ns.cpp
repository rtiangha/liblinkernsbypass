// SPDX-License-Identifier: BSD-2-Clause
// Copyright © 2021 Billy Laws

#include <array>
#include <cstdio>
#include <cstdlib>
#include <errno.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <android/log.h>
#include "elf_soname_patcher.h"
#include "android_linker_ns.h"

using loader_android_create_namespace_t = android_namespace_t *(*)(const char *, const char *, const char *, uint64_t, const char *, android_namespace_t *, const void *);
static loader_android_create_namespace_t loader_android_create_namespace;

static bool lib_loaded;

/* Public API */
bool linkernsbypass_load_status() {
    return lib_loaded;
}

struct android_namespace_t *android_create_namespace(const char *name,
                                                     const char *ld_library_path,
                                                     const char *default_library_path,
                                                     uint64_t type,
                                                     const char *permitted_when_isolated_path,
                                                     struct android_namespace_t *parent_namespace) {
    auto caller_addr{__builtin_return_address(0)};
    return loader_android_create_namespace(name, ld_library_path, default_library_path, type,
                                           permitted_when_isolated_path, parent_namespace, caller_addr);
}

struct android_namespace_t *android_create_namespace_escape(const char *name,
                                                            const char *ld_library_path,
                                                            const char *default_library_path,
                                                            uint64_t type,
                                                            const char *permitted_when_isolated_path,
                                                            struct android_namespace_t *parent_namespace) {
    auto caller_addr{reinterpret_cast<void *>(&dlopen)};
    return loader_android_create_namespace(name, ld_library_path, default_library_path, type,
                                           permitted_when_isolated_path, parent_namespace, caller_addr);
}

android_get_exported_namespace_t android_get_exported_namespace;

android_link_namespaces_all_libs_t android_link_namespaces_all_libs;

android_link_namespaces_t android_link_namespaces;

// We have to resolve android_dlopen_ext ourselves as a hook library could overwrite it and then call dlopen_hooked
decltype(&android_dlopen_ext) libdl_android_dlopen_ext;

bool linkernsbypass_namespace_apply_hook(const char *hookLibName, struct android_namespace_t *hookNs, const void *hookParam) {
    android_dlextinfo hookExtInfo{
        .flags = ANDROID_DLEXT_USE_NAMESPACE,
        .library_namespace = hookNs
    };

    // Load hook syms into the NS as global
    auto hookLib{libdl_android_dlopen_ext(hookLibName, RTLD_GLOBAL, &hookExtInfo)};
    if (!hookLib)
        return false;

    // Set optional parameter
    auto hookParamSym{reinterpret_cast<const void **>(dlsym(hookLib, "hook_param"))};
    if (hookParamSym || hookParam) {
        if (hookParamSym && hookParam)
            *hookParamSym = hookParam;
        else
            return false;
    }

    return true;
}

#ifndef __NR_memfd_create
    #if defined(__aarch64__)
        #define __NR_memfd_create 279
    #else
        #error Unsupported target architecture!
    #endif
#endif

void *linkernsbypass_dlopen_unique_hooked(const char *libPath, const char *libTargetDir, int mode, const char *hookLibDir, const char *hookLibName, struct android_namespace_t *parentNs, bool linkToDefault, const void *hookParam) {
    static std::array<char, PATH_MAX> PathBuf{};

    // Create a namespace that can isolate our hook from the default
    auto hookNs{android_create_namespace(libPath, hookLibDir, nullptr, ANDROID_NAMESPACE_TYPE_SHARED, nullptr, parentNs)};

    if (linkToDefault) {
        auto defaultNs{android_create_namespace_escape("default_copy", nullptr, nullptr, ANDROID_NAMESPACE_TYPE_SHARED, nullptr, nullptr)};
        android_link_namespaces_all_libs(hookNs, defaultNs);
    }

    android_link_namespaces_all_libs(hookNs, parentNs);

    if (hookLibName) {
        if (!linkernsbypass_namespace_apply_hook(hookLibName, hookNs, hookParam))
            return nullptr;
    } else {
        if (hookParam)
            return nullptr;
    }

    // Used as a unique ID for overwriting soname and creating target lib files
    static uint16_t TargetId{};

    int libTargetFd{[&] () {
        if (libTargetDir) {
            snprintf(PathBuf.data(), PathBuf.size(), "%s/%d_patched.so", libTargetDir, TargetId);
            return open(PathBuf.data(), O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
        } else {
            // If memfd isn't supported errno will contain ENOSYS after calling
            errno = 0;
            int fd{static_cast<int>(syscall(__NR_memfd_create, libPath, 0))};
            if (errno == ENOSYS || fd < 0)
                return -1;
            else
                return fd;
        }
    }()};
    if (libTargetFd == -1)
        return nullptr;

    // Partially overwrite soname with 3 digits (replacing lib...) with to make sure a cached so isn't loaded
    std::array<char, 3> sonameOverwrite{};
    snprintf(sonameOverwrite.data(), sonameOverwrite.size(), "%03u", TargetId++);

    if (!elf_soname_patch(libPath, libTargetFd, sonameOverwrite.data()))
        return nullptr;

    // Load our patched library into the hook namespace
    android_dlextinfo hookExtInfo{
        .flags = ANDROID_DLEXT_USE_NAMESPACE | ANDROID_DLEXT_USE_LIBRARY_FD,
        .library_fd = libTargetFd,
        .library_namespace = hookNs
    };

    // Make a path that looks about right
    snprintf(PathBuf.data(), PathBuf.size(), "/proc/self/fd/%d", libTargetFd);

    return libdl_android_dlopen_ext(PathBuf.data(), mode, &hookExtInfo);
}

/* Private */
__attribute__((constructor)) static void resolve_linker_symbols() {
    using loader_dlopen_t = void *(*)(const char *, int, const void *);

    // ARM64 specific function walking to locate the internal dlopen handler
    auto loader_dlopen{[]() {
        union BranchLinked {
            uint32_t raw;

            struct {
                int32_t offset : 26; //!< 26-bit branch offset
                uint8_t sig : 6;  //!< 6-bit signature
            };

            bool Verify() {
                return sig == 0x25;
            }
        };
        static_assert(sizeof(BranchLinked) == 4, "BranchLinked is wrong size");

        // dlopen is just a wrapper for __loader_dlopen that passes the return address as the third arg hence we can just walk it to find __loader_dlopen
        auto blInstr{reinterpret_cast<BranchLinked *>(&dlopen)};
        while (!blInstr->Verify())
            blInstr++;

        return reinterpret_cast<loader_dlopen_t>(blInstr + blInstr->offset);
    }()};

    // Passing dlopen as a caller address tricks the linker into using the internal unrestricted namespace letting us access libraries that are normally forbidden in the classloader namespace imposed on apps
    auto ldHandle{loader_dlopen("ld-android.so", RTLD_LAZY, reinterpret_cast<void *>(&dlopen))};
    if (!ldHandle)
        return;

    android_link_namespaces_all_libs = reinterpret_cast<android_link_namespaces_all_libs_t>(dlsym(ldHandle, "__loader_android_link_namespaces_all_libs"));
    if (!android_link_namespaces_all_libs)
        return;

    android_link_namespaces = reinterpret_cast<android_link_namespaces_t>(dlsym(ldHandle, "__loader_android_link_namespaces"));
    if (!android_link_namespaces)
        return;

    auto libdlAndroidHandle{loader_dlopen("libdl_android.so", RTLD_LAZY, reinterpret_cast<void *>(&dlopen))};
    if (!libdlAndroidHandle)
        return;

    loader_android_create_namespace = reinterpret_cast<loader_android_create_namespace_t>(dlsym(libdlAndroidHandle, "__loader_android_create_namespace"));
    if (!loader_android_create_namespace)
        return;

    android_get_exported_namespace = reinterpret_cast<android_get_exported_namespace_t>(dlsym(libdlAndroidHandle, "__loader_android_get_exported_namespace"));
    if (!android_get_exported_namespace)
        return;

    auto libdlHandle{dlopen("libdl.so", RTLD_LAZY)};
    if (!libdlHandle)
        return;

    libdl_android_dlopen_ext = reinterpret_cast<decltype(&android_dlopen_ext)>(dlsym(libdlHandle, "android_dlopen_ext"));
    if (!libdl_android_dlopen_ext)
        return;

    // Lib is now safe to use
    lib_loaded = true;
}
