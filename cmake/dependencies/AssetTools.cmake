include_guard(GLOBAL)

# KTX is both a runtime reader and an offline authoring dependency. The
# renderer always needs KTX2 read support; KTX1, the CLI and DirectXTex are
# configured only when VulkanLabAssetTool is part of this generation tree.
# Upstream exposes generic cache names, so preserve their caller-visible
# values after its targets have been configured.
set(_VKL_GENERIC_CACHE_ENTRIES BUILD_SHARED_LIBS)
if(VKL_BUILD_ASSET_TOOL)
    list(APPEND _VKL_GENERIC_CACHE_ENTRIES
        BUILD_TOOLS BUILD_SAMPLE BUILD_DX11 BUILD_DX12 BC_USE_OPENMP)
endif()
foreach(_VKL_CACHE_ENTRY IN LISTS _VKL_GENERIC_CACHE_ENTRIES)
    get_property(_VKL_CACHE_${_VKL_CACHE_ENTRY}_SET
        CACHE ${_VKL_CACHE_ENTRY} PROPERTY TYPE SET)
    if(_VKL_CACHE_${_VKL_CACHE_ENTRY}_SET)
        get_property(_VKL_CACHE_${_VKL_CACHE_ENTRY}_TYPE
            CACHE ${_VKL_CACHE_ENTRY} PROPERTY TYPE)
        get_property(_VKL_CACHE_${_VKL_CACHE_ENTRY}_VALUE
            CACHE ${_VKL_CACHE_ENTRY} PROPERTY VALUE)
    endif()
endforeach()

# KTX defaults enable tests, documentation and graphics upload helpers. None
# belong to VulkanLab's runtime or authoring workflow, so override them before
# add_subdirectory rather than leaking upstream defaults into every profile.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static dependencies" FORCE)
set(KTX_FEATURE_TESTS OFF CACHE BOOL "Disable KTX tests" FORCE)
set(KTX_FEATURE_DOC OFF CACHE BOOL "Disable KTX documentation" FORCE)
set(KTX_FEATURE_JNI OFF CACHE BOOL "Disable KTX Java bindings" FORCE)
set(KTX_FEATURE_PY OFF CACHE BOOL "Disable KTX Python bindings" FORCE)
set(KTX_FEATURE_GL_UPLOAD OFF CACHE BOOL "Disable KTX OpenGL upload" FORCE)
set(KTX_FEATURE_VK_UPLOAD OFF CACHE BOOL "Disable KTX Vulkan upload" FORCE)
set(KTX_FEATURE_LOADTEST_APPS OFF CACHE STRING "Disable KTX load tests" FORCE)
set(KTX_FEATURE_KTX1 ${VKL_BUILD_ASSET_TOOL}
    CACHE BOOL "Required by KTX 4.4.2 tools" FORCE)
set(KTX_FEATURE_KTX2 ON CACHE BOOL "Enable KTX2" FORCE)
set(KTX_FEATURE_TOOLS ${VKL_BUILD_ASSET_TOOL}
    CACHE BOOL "Build KTX command-line tools" FORCE)
set(KTX_FEATURE_TOOLS_CTS OFF CACHE BOOL "Disable KTX tools CTS" FORCE)
add_subdirectory("${PROJECT_SOURCE_DIR}/external/ktx" external/ktx
                 EXCLUDE_FROM_ALL)

if(VKL_BUILD_ASSET_TOOL)
    # DirectXTex is an offline-only dependency used to produce native BC7.
    # VulkanLab owns texture-level scheduling, so upstream tools, graphics
    # helpers, samples and OpenMP are disabled deliberately.
    set(BUILD_TOOLS OFF CACHE BOOL "Disable DirectXTex tools" FORCE)
    set(BUILD_SAMPLE OFF CACHE BOOL "Disable DirectXTex samples" FORCE)
    set(BUILD_DX11 OFF CACHE BOOL "Disable DirectXTex D3D11 helpers" FORCE)
    set(BUILD_DX12 OFF CACHE BOOL "Disable DirectXTex D3D12 helpers" FORCE)
    set(BC_USE_OPENMP OFF CACHE BOOL
        "Texture-level scheduling owns BC7 parallelism" FORCE)
    add_subdirectory("${PROJECT_SOURCE_DIR}/external/DirectXTex"
                     external/DirectXTex EXCLUDE_FROM_ALL)
endif()

foreach(_VKL_CACHE_ENTRY IN LISTS _VKL_GENERIC_CACHE_ENTRIES)
    if(_VKL_CACHE_${_VKL_CACHE_ENTRY}_SET)
        set(${_VKL_CACHE_ENTRY} "${_VKL_CACHE_${_VKL_CACHE_ENTRY}_VALUE}"
            CACHE ${_VKL_CACHE_${_VKL_CACHE_ENTRY}_TYPE}
            "Restored after VulkanLab dependency configuration" FORCE)
    else()
        unset(${_VKL_CACHE_ENTRY} CACHE)
    endif()
endforeach()

# KTX 4.4.2's read-only target still references the KTX1 constructor from
# its format dispatcher when KTX1 is disabled. Supply the documented
# unsupported-feature result in runtime-only profiles.
if(NOT KTX_FEATURE_KTX1)
    target_sources(ktx_read PRIVATE
        "${PROJECT_SOURCE_DIR}/src/third_party/Ktx1DisabledStub.c"
    )
    target_include_directories(ktx_read PRIVATE
        "${PROJECT_SOURCE_DIR}/external/ktx/lib"
    )
endif()

# KTX registers CLI tests whenever its tools are enabled, including tools this
# project intentionally does not build. Keep CTest focused on VulkanLab.
get_property(KTX_REGISTERED_TESTS
    DIRECTORY "${PROJECT_SOURCE_DIR}/external/ktx/tests"
    PROPERTY TESTS)
if(KTX_REGISTERED_TESTS)
    set(KTX_CTEST_IGNORE_FILE "${PROJECT_BINARY_DIR}/CTestCustom.cmake")
    file(WRITE "${KTX_CTEST_IGNORE_FILE}" "set(CTEST_CUSTOM_TESTS_IGNORE\n")
    foreach(KTX_REGISTERED_TEST IN LISTS KTX_REGISTERED_TESTS)
        file(APPEND "${KTX_CTEST_IGNORE_FILE}"
            "  \"${KTX_REGISTERED_TEST}\"\n")
    endforeach()
    file(APPEND "${KTX_CTEST_IGNORE_FILE}" ")\n")
endif()

# astc-encoder defaults to the static MSVC runtime. Match VulkanLab's /MD
# boundary to avoid crossing incompatible CRT heaps.
if(MSVC)
    foreach(ASTC_TARGET
            astcenc-avx2-static
            astcenc-sse4.1-static
            astcenc-sse2-static
            astcenc-neon-static
            astcenc-native-static
            astcenc-none-static)
        if(TARGET ${ASTC_TARGET})
            set_property(TARGET ${ASTC_TARGET} PROPERTY MSVC_RUNTIME_LIBRARY
                "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        endif()
    endforeach()
endif()
