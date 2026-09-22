cmake_minimum_required(VERSION 3.16)
project(dusktracer LANGUAGES CXX)

option(DUSK_BUILD_GRAPH_TESTS "Build material graph evaluation and ImGui interaction tests" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_BUILD_TYPE Release)
set(CMAKE_CXX_FLAGS_RELEASE "/O2 /DNDEBUG")

# Enable AddressSanitizer for MSVC Debug builds to pinpoint memory errors
if (MSVC)
    # Ensure all targets use the dynamic runtime in Debug (ASan requires /MDd)
    # Apply to targets after creation as well (we also set it again per-target below)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# Headers (your core engine)
include_directories(${CMAKE_SOURCE_DIR}/include)

# ---------------------------------------------------------------------------
# ImGui (from external/imgui)
# ---------------------------------------------------------------------------
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/external/imgui)

set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp          # optional but useful while developing
    ${IMGUI_DIR}/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/imgui_impl_opengl3.cpp
)

add_library(imgui STATIC ${IMGUI_SOURCES})

target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
)

if (MSVC)
    # Match runtime library; avoid LNK2038 CRT mismatch in Debug
    set_property(TARGET imgui PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# ---------------------------------------------------------------------------
# GLFW: headers + prebuilt libs in include/external
# ---------------------------------------------------------------------------
# Layout:
#   include/external/GLFW/glfw3.h
#   include/external/GLFW/glfw3native.h
#   include/external/glfw3.lib
#   include/external/glfw3.dll
set(GLFW_ROOT        "${CMAKE_SOURCE_DIR}/include/external")
set(GLFW_INCLUDE_DIR "${GLFW_ROOT}")
set(GLFW_LIB_PATH    "${GLFW_ROOT}/glfw3.lib")   # use glfw3_mt.lib if you prefer

# Make sure everyone can see <GLFW/glfw3.h>
include_directories(${GLFW_INCLUDE_DIR})
target_include_directories(imgui PUBLIC ${GLFW_INCLUDE_DIR})

# ---------------------------------------------------------------------------
# GLEW: headers + prebuilt libs in external/glew
# ---------------------------------------------------------------------------
# Expected layout:
#   external/glew/include/GL/glew.h
#   external/glew/lib/Release/x64/glew32.lib
#   external/glew/bin/glew32.dll
set(GLEW_ROOT         "${CMAKE_SOURCE_DIR}/external/glew")
set(GLEW_INCLUDE_DIR  "${GLEW_ROOT}/include")
set(GLEW_LIB_PATH     "${GLEW_ROOT}/lib/Release/x64/glew32.lib")

include_directories(${GLEW_INCLUDE_DIR})
target_include_directories(imgui PUBLIC ${GLEW_INCLUDE_DIR})

# ---------------------------------------------------------------------------
# Core sources (everything except the two mains)
# ---------------------------------------------------------------------------
file(GLOB_RECURSE DUSK_SOURCES CONFIGURE_DEPENDS
    "src/core/*.cpp"
)

# Remove main.cpp and editor_main.cpp from the shared source list
list(REMOVE_ITEM DUSK_SOURCES
    "${CMAKE_SOURCE_DIR}/src/core/main.cpp"
    "${CMAKE_SOURCE_DIR}/src/core/editor_main.cpp"
)

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------

# Single editor executable (renamed to `Dusktracer`)
add_executable(Dusktracer
    src/core/editor_main.cpp
    ${DUSK_SOURCES}
)

if (MSVC)
    # AddressSanitizer only for Debug configuration
    target_compile_options(Dusktracer PRIVATE
        $<$<CONFIG:Debug>:/fsanitize=address>
        $<$<CONFIG:Debug>:/Zi>
        $<$<CONFIG:Debug>:/Od>
    )
    target_link_options(Dusktracer PRIVATE
        $<$<CONFIG:Debug>:/fsanitize=address>
        $<$<CONFIG:Debug>:/DEBUG:FULL>
        $<$<CONFIG:Debug>:/INCREMENTAL:NO>
    )
    # Ensure the expected runtime in Debug for ASan
    set_property(TARGET Dusktracer PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

    # Configure ASan logging and a helper run target
    set(ASAN_LOG_DIR "${CMAKE_BINARY_DIR}/asan-logs")
    file(MAKE_DIRECTORY "${ASAN_LOG_DIR}")
    set(_ASAN_ENV "ASAN_OPTIONS=log_path=${ASAN_LOG_DIR}/asan;fast_unwind_on_fatal=0;handle_abort=1;handle_segv=1;halt_on_error=1;verbosity=1")
    set(_OMP_ENV "OMP_NUM_THREADS=1")

    # When launching under Visual Studio, set environment and working dir
    set_property(TARGET Dusktracer PROPERTY VS_DEBUGGER_ENVIRONMENT "${_ASAN_ENV}\n${_OMP_ENV}")
    set_property(TARGET Dusktracer PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

    # Convenience target to run with ASan env from CMake/CLI
    add_custom_target(ASanRun
        COMMAND ${CMAKE_COMMAND} -E env ${_ASAN_ENV} ${_OMP_ENV} $<TARGET_FILE:Dusktracer>
        DEPENDS Dusktracer
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        VERBATIM
    )
endif()

# ---------------------------------------------------------------------------
# Windows icon resource (for EXE/taskbar/titlebar)
# ---------------------------------------------------------------------------
if (WIN32)
    # Single RC used by both targets. Path is relative to CMakeLists.txt.
    set(APP_ICON_RC "${CMAKE_SOURCE_DIR}/appicon.rc")

    target_sources(Dusktracer        PRIVATE ${APP_ICON_RC})
endif()

# ---------------------------------------------------------------------------
# OpenMP
# ---------------------------------------------------------------------------
find_package(OpenMP)
if (OpenMP_CXX_FOUND)
    target_link_libraries(Dusktracer PRIVATE OpenMP::OpenMP_CXX)
endif()

# ---------------------------------------------------------------------------
# Assimp (Release)
# ---------------------------------------------------------------------------
set(ASSIMP_ROOT        "${CMAKE_SOURCE_DIR}/external/assimp")
set(ASSIMP_INCLUDE_DIR "${ASSIMP_ROOT}/include")
set(ASSIMP_LIB_RELEASE "${ASSIMP_ROOT}/lib/assimp-vc143-mt.lib")

target_include_directories(Dusktracer PRIVATE ${ASSIMP_INCLUDE_DIR})
target_link_libraries(Dusktracer PRIVATE ${ASSIMP_LIB_RELEASE})

# ---------------------------------------------------------------------------
# OpenImageDenoise (optional)
# Auto-detect SDK under external/OpenImageDenoise so a plain `cmake ..` works
# ---------------------------------------------------------------------------
set(OIDN_SDK_DIR "${CMAKE_SOURCE_DIR}/external/OpenImageDenoise")
set(OIDN_SDK_EXISTS FALSE)
if (EXISTS "${OIDN_SDK_DIR}/include" AND EXISTS "${OIDN_SDK_DIR}/lib")
    set(OIDN_SDK_EXISTS TRUE)
endif()

# ---------------------------------------------------------------------------
# Embree (optional)
# Auto-detect SDK under external/embree so a plain `cmake ..` works
# ---------------------------------------------------------------------------
set(EMBREE_SDK_DIR "${CMAKE_SOURCE_DIR}/external/embree")
set(EMBREE_SDK_EXISTS FALSE)
if (EXISTS "${EMBREE_SDK_DIR}/include" AND EXISTS "${EMBREE_SDK_DIR}/lib")
    set(EMBREE_SDK_EXISTS TRUE)
endif()

# Default USE_EMBREE to ON. Configure will fail if Embree is requested but not found.
option(USE_EMBREE "Enable Intel Embree acceleration support" ON)
if (USE_EMBREE)
    # Prefer embree3 layout but also accept embree4 (some SDK distributions use embree4/)
    find_path(EMBREE_INCLUDE_DIR NAMES "embree3/rtcore.h" PATHS "${EMBREE_SDK_DIR}/include" NO_DEFAULT_PATH)
    if (NOT EMBREE_INCLUDE_DIR)
        find_path(EMBREE_INCLUDE_DIR NAMES "embree4/rtcore.h" PATHS "${EMBREE_SDK_DIR}/include" NO_DEFAULT_PATH)
    endif()

    # Try common library names including embree4.lib on Windows
    find_library(EMBREE_LIBRARY NAMES embree4 embree3 Embree4 Embree3 rtcore PATHS "${EMBREE_SDK_DIR}/lib" NO_DEFAULT_PATH)

    if (NOT EMBREE_INCLUDE_DIR)
        find_path(EMBREE_INCLUDE_DIR NAMES "embree3/rtcore.h")
    endif()
    if (NOT EMBREE_LIBRARY)
        find_library(EMBREE_LIBRARY NAMES embree3 Embree3 rtcore)
    endif()

    if (EMBREE_INCLUDE_DIR AND EMBREE_LIBRARY)
        add_library(Embree::Embree UNKNOWN IMPORTED)
        set_target_properties(Embree::Embree PROPERTIES
            IMPORTED_LOCATION "${EMBREE_LIBRARY}" )
        target_include_directories(Embree::Embree INTERFACE ${EMBREE_INCLUDE_DIR})
        target_link_libraries(Dusktracer PRIVATE Embree::Embree)
        target_compile_definitions(Dusktracer PRIVATE HAVE_EMBREE=1)
        message(STATUS "Embree found: ${EMBREE_LIBRARY}")
    else()
        message(FATAL_ERROR "USE_EMBREE=ON but Embree not found. Please install Embree into 'external/embree' or re-run CMake with -DUSE_EMBREE=OFF to proceed without Embree.")
    endif()
endif()

# Default USE_OIDN to ON when the SDK folder is present so plain `cmake ..` enables it.
option(USE_OIDN "Enable OpenImageDenoise support" ${OIDN_SDK_EXISTS})
if (USE_OIDN)
    # Prefer explicit SDK subfolders, fall back to system search if not found there.
    find_path(OIDN_INCLUDE_DIR NAMES "OpenImageDenoise/oidn.hpp" PATHS "${OIDN_SDK_DIR}/include" NO_DEFAULT_PATH)
    find_library(OIDN_LIBRARY NAMES OpenImageDenoise OpenImageDenoise.lib oidn oidn.lib PATHS "${OIDN_SDK_DIR}/lib" NO_DEFAULT_PATH)

    if (NOT OIDN_INCLUDE_DIR)
        find_path(OIDN_INCLUDE_DIR NAMES "OpenImageDenoise/oidn.hpp")
    endif()
    if (NOT OIDN_LIBRARY)
        find_library(OIDN_LIBRARY NAMES OpenImageDenoise oidn PATHS "${CMAKE_SOURCE_DIR}/external/" ENV PATH)
    endif()

    if (OIDN_INCLUDE_DIR AND OIDN_LIBRARY)
        add_library(OpenImageDenoise::OpenImageDenoise UNKNOWN IMPORTED)
        set_target_properties(OpenImageDenoise::OpenImageDenoise PROPERTIES
            IMPORTED_LOCATION "${OIDN_LIBRARY}" )
        target_include_directories(OpenImageDenoise::OpenImageDenoise INTERFACE ${OIDN_INCLUDE_DIR})
        target_link_libraries(Dusktracer PRIVATE OpenImageDenoise::OpenImageDenoise)
        target_compile_definitions(Dusktracer PRIVATE HAVE_OIDN=1)
        message(STATUS "OpenImageDenoise found: ${OIDN_LIBRARY}")
    else()
        message(WARNING "USE_OIDN=ON but OpenImageDenoise not found. Denoiser will be disabled.")
    endif()
endif()

# ---------------------------------------------------------------------------
# OpenGL + link ImGui + GLFW + GLEW into the editor
# ---------------------------------------------------------------------------
find_package(OpenGL REQUIRED)

target_link_libraries(Dusktracer
    PRIVATE
        imgui
        ${GLFW_LIB_PATH}
        ${GLEW_LIB_PATH}
        OpenGL::GL
)

# ---------------------------------------------------------------------------
# Optional ISPC kernels
# ---------------------------------------------------------------------------
option(USE_ISPC "Enable ISPC kernels" ON)
if (USE_ISPC)
    find_program(ISPC_EXECUTABLE NAMES ispc HINTS $ENV{PATH})
    if (ISPC_EXECUTABLE)
        message(STATUS "ISPC found: ${ISPC_EXECUTABLE}")
        set(ISPC_OUT_DIR "${CMAKE_BINARY_DIR}/ispc")
        file(MAKE_DIRECTORY ${ISPC_OUT_DIR})

        set(ISPC_SRC "${CMAKE_SOURCE_DIR}/src/ispc/brdf.ispc")
        if (WIN32)
            set(ISPC_OBJ "${ISPC_OUT_DIR}/brdf_ispc.obj")
        else()
            set(ISPC_OBJ "${ISPC_OUT_DIR}/brdf_ispc.o")
        endif()
        set(ISPC_HEADER "${ISPC_OUT_DIR}/brdf_ispc.h")

        # Allow users to override ISPC flags (empty by default). Use e.g.
        # -DISPC_FLAGS="--target=x86-64" or -DISPC_FLAGS="--target=avx2" depending on your ispc version.
        if (NOT DEFINED ISPC_FLAGS)
            set(ISPC_FLAGS "")
        endif()

        add_custom_command(
            OUTPUT ${ISPC_OBJ} ${ISPC_HEADER}
            COMMAND ${ISPC_EXECUTABLE} ${ISPC_SRC} -o ${ISPC_OBJ} -h ${ISPC_HEADER} ${ISPC_FLAGS}
            DEPENDS ${ISPC_SRC}
            COMMENT "Compiling ISPC kernels: ${ISPC_SRC} -> ${ISPC_OBJ}"
            VERBATIM
        )

        add_custom_target(ispc_kernels DEPENDS ${ISPC_OBJ} ${ISPC_HEADER})

        # Ensure include dir for generated header and link the object into the final target
        target_include_directories(Dusktracer PRIVATE ${ISPC_OUT_DIR})
        target_sources(Dusktracer PRIVATE ${ISPC_OBJ})
        add_dependencies(Dusktracer ispc_kernels)
        target_compile_definitions(Dusktracer PRIVATE HAVE_ISPC=1)
    else()
        # If ispc wasn't found on PATH, try the common Program Files installation
        if (WIN32)
            # Try common Program Files locations and environment variables
            if (EXISTS "$ENV{ProgramFiles}/ispc/ispc.exe")
                set(ISPC_EXECUTABLE "$ENV{ProgramFiles}/ispc/ispc.exe")
            elseif (EXISTS "$ENV{PROGRAMFILES}/ispc/ispc.exe")
                set(ISPC_EXECUTABLE "$ENV{PROGRAMFILES}/ispc/ispc.exe")
            elseif (EXISTS "C:/Program Files/ispc/ispc.exe")
                set(ISPC_EXECUTABLE "C:/Program Files/ispc/ispc.exe")
            elseif (EXISTS "C:/Program Files (x86)/ispc/ispc.exe")
                set(ISPC_EXECUTABLE "C:/Program Files (x86)/ispc/ispc.exe")
            endif()

            if (ISPC_EXECUTABLE)
                message(STATUS "ISPC found at: ${ISPC_EXECUTABLE}")
                set(ISPC_OUT_DIR "${CMAKE_BINARY_DIR}/ispc")
                file(MAKE_DIRECTORY ${ISPC_OUT_DIR})

                set(ISPC_SRC "${CMAKE_SOURCE_DIR}/src/ispc/brdf.ispc")
                if (WIN32)
                    set(ISPC_OBJ "${ISPC_OUT_DIR}/brdf_ispc.obj")
                else()
                    set(ISPC_OBJ "${ISPC_OUT_DIR}/brdf_ispc.o")
                endif()
                set(ISPC_HEADER "${ISPC_OUT_DIR}/brdf_ispc.h")

                if (NOT DEFINED ISPC_FLAGS)
                    set(ISPC_FLAGS "")
                endif()

                add_custom_command(
                    OUTPUT ${ISPC_OBJ} ${ISPC_HEADER}
                    COMMAND ${ISPC_EXECUTABLE} ${ISPC_SRC} -o ${ISPC_OBJ} -h ${ISPC_HEADER} ${ISPC_FLAGS}
                    DEPENDS ${ISPC_SRC}
                    COMMENT "Compiling ISPC kernels: ${ISPC_SRC} -> ${ISPC_OBJ}"
                    VERBATIM
                )

                add_custom_target(ispc_kernels DEPENDS ${ISPC_OBJ} ${ISPC_HEADER})

                target_include_directories(Dusktracer PRIVATE ${ISPC_OUT_DIR})
                target_sources(Dusktracer PRIVATE ${ISPC_OBJ})
                add_dependencies(Dusktracer ispc_kernels)
                target_compile_definitions(Dusktracer PRIVATE HAVE_ISPC=1)
            else()
                message(WARNING "ISPC requested (USE_ISPC=ON) but `ispc` not found in PATH or common Program Files locations. Building without ISPC kernels.")
            endif()
        else()
            message(WARNING "ISPC requested (USE_ISPC=ON) but `ispc` not found in PATH. Building without ISPC kernels.")
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# Post-build: copy DLL/models/textures for main renderer
# ---------------------------------------------------------------------------
# Post-build: copy DLL/models/textures next to the executable
add_custom_command(TARGET Dusktracer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${ASSIMP_ROOT}/bin/assimp-vc143-mt.dll"
        "$<TARGET_FILE_DIR:Dusktracer>"
)

add_custom_command(TARGET Dusktracer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/models"
        "$<TARGET_FILE_DIR:Dusktracer>/models"
)

add_custom_command(TARGET Dusktracer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/textures"
        "$<TARGET_FILE_DIR:Dusktracer>/textures"
)

# Copy application icon PNG next to exe so runtime can find it by relative path
add_custom_command(TARGET Dusktracer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/resources/dusktracer.png"
        "$<TARGET_FILE_DIR:Dusktracer>"
)

# Copy glfw3.dll next to the editor exe (if using dynamic GLFW)
# Copy glfw3.dll next to the exe (if using dynamic GLFW)
add_custom_command(TARGET Dusktracer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${GLFW_ROOT}/glfw3.dll"
        "$<TARGET_FILE_DIR:Dusktracer>"
)

# Copy glew32.dll next to the exe
add_custom_command(TARGET Dusktracer POST_BUILD         
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${GLEW_ROOT}/bin/glew32.dll"
        "$<TARGET_FILE_DIR:Dusktracer>"
)

# Ensure a `Renders` directory exists next to the built executables (useful for runtime saves)
add_custom_command(TARGET Dusktracer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:Dusktracer>/Renders"
)

add_custom_command(TARGET Dusktracer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:Dusktracer>/Renders"
)

# ---------------------------------------------------------------------------
# Post-build: copy OpenImageDenoise runtime DLLs if available
# ---------------------------------------------------------------------------
if (EXISTS "${CMAKE_SOURCE_DIR}/external/OpenImageDenoise/bin")
    add_custom_command(TARGET Dusktracer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/external/OpenImageDenoise/bin"
            "$<TARGET_FILE_DIR:Dusktracer>"
    )
endif()

# ---------------------------------------------------------------------------
# Post-build: copy Embree runtime DLLs if available
# ---------------------------------------------------------------------------
if (EXISTS "${CMAKE_SOURCE_DIR}/external/embree/bin")
    add_custom_command(TARGET Dusktracer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/external/embree/bin"
            "$<TARGET_FILE_DIR:Dusktracer>"
    )
endif()

option(DUSK_ENABLE_VULKAN_RT "Build optional Vulkan hardware ray-query backend when the SDK is available" ON)
if (DUSK_ENABLE_VULKAN_RT)
    find_package(Vulkan QUIET)
    find_program(DUSK_GLSLC NAMES glslc HINTS "$ENV{VULKAN_SDK}/Bin")
    if (Vulkan_FOUND AND DUSK_GLSLC)
        set(DUSK_SPV "${CMAKE_BINARY_DIR}/generated/pathtrace.spv")
        set(DUSK_SPV_HEADER "${CMAKE_BINARY_DIR}/generated/pathtrace_spv.h")
        add_custom_command(OUTPUT "${DUSK_SPV_HEADER}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
            COMMAND "${DUSK_GLSLC}" --target-env=vulkan1.2 -O
                "${CMAKE_SOURCE_DIR}/src/gpu/shaders/pathtrace.comp" -o "${DUSK_SPV}"
            COMMAND ${CMAKE_COMMAND} "-DINPUT=${DUSK_SPV}" "-DOUTPUT=${DUSK_SPV_HEADER}"
                -P "${CMAKE_SOURCE_DIR}/cmake/EmbedSpirv.cmake"
            DEPENDS src/gpu/shaders/pathtrace.comp cmake/EmbedSpirv.cmake VERBATIM)
        add_library(dusk_vulkan_rt STATIC src/gpu/vulkan_rt.cpp "${DUSK_SPV_HEADER}")
        target_include_directories(dusk_vulkan_rt PRIVATE ${Vulkan_INCLUDE_DIRS} "${CMAKE_BINARY_DIR}/generated")
        # Load the system Vulkan loader dynamically: a missing driver must never
        # prevent the editor or the CPU backend from starting.
        if (TARGET OpenImageDenoise::OpenImageDenoise)
            target_link_libraries(dusk_vulkan_rt PRIVATE OpenImageDenoise::OpenImageDenoise)
            target_compile_definitions(dusk_vulkan_rt PRIVATE HAVE_OIDN=1)
        endif()
        target_link_libraries(Dusktracer PRIVATE dusk_vulkan_rt)
        target_compile_definitions(Dusktracer PRIVATE DUSK_ENABLE_VULKAN_RT=1)
        message(STATUS "Vulkan hardware ray queries enabled")
    else()
        message(STATUS "Vulkan SDK/glslc unavailable: building CPU backend only")
    endif()
endif()

if (DUSK_BUILD_GRAPH_TESTS)
    enable_testing()
    add_executable(DuskPreviewWorkerTests tests/preview_worker_tests.cpp)
    add_test(NAME preview_worker_scheduling COMMAND DuskPreviewWorkerTests)
    add_executable(DuskGraphTests tests/material_graph_tests.cpp
        src/core/material_graph.cpp src/core/scene.cpp src/core/dusk_image.cpp
        src/core/interval.cpp src/core/aabb.cpp src/core/materials/sss.cpp)
    target_include_directories(DuskGraphTests PRIVATE ${ASSIMP_INCLUDE_DIR})
    target_link_libraries(DuskGraphTests PRIVATE ${ASSIMP_LIB_RELEASE})
    add_test(NAME material_graph_evaluation COMMAND DuskGraphTests)

    add_executable(DuskGraphEditorTests tests/material_graph_editor_tests.cpp
        src/core/material_graph.cpp src/core/dusk_image.cpp src/core/interval.cpp src/core/undo.cpp)
    target_link_libraries(DuskGraphEditorTests PRIVATE imgui ${GLFW_LIB_PATH} ${GLEW_LIB_PATH} OpenGL::GL)
    add_test(NAME material_graph_interactions COMMAND DuskGraphEditorTests "${CMAKE_BINARY_DIR}/material-graph-preview.png")

    add_executable(DuskGraphIntegrationTests tests/material_graph_integration_tests.cpp ${DUSK_SOURCES})
    add_executable(DuskPbrTests tests/pbr_tests.cpp ${DUSK_SOURCES})
    add_executable(DuskGpuTests tests/gpu_tests.cpp ${DUSK_SOURCES})
    if (TARGET dusk_vulkan_rt)
        target_include_directories(DuskGpuTests PRIVATE ${Vulkan_INCLUDE_DIRS})
    endif()
    foreach(test_target DuskGraphIntegrationTests DuskPbrTests DuskGpuTests)
        target_include_directories(${test_target} PRIVATE $<TARGET_PROPERTY:Dusktracer,INCLUDE_DIRECTORIES>)
        target_compile_definitions(${test_target} PRIVATE $<TARGET_PROPERTY:Dusktracer,COMPILE_DEFINITIONS>)
        target_link_libraries(${test_target} PRIVATE $<TARGET_PROPERTY:Dusktracer,LINK_LIBRARIES>)
        if (TARGET ispc_kernels)
            target_sources(${test_target} PRIVATE ${ISPC_OBJ})
            add_dependencies(${test_target} ispc_kernels)
        endif()
    endforeach()
    add_test(NAME pbr_transport_correctness COMMAND DuskPbrTests "${CMAKE_BINARY_DIR}/pbr-validation.png")
    add_test(NAME gpu_transport_correctness COMMAND DuskGpuTests "${CMAKE_BINARY_DIR}/gpu-validation.png")
    set_tests_properties(gpu_transport_correctness PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME material_graph_editor_integration COMMAND DuskGraphIntegrationTests "${CMAKE_BINARY_DIR}/graph-integration")
    add_test(NAME raster_materials_and_shadows COMMAND DuskGraphIntegrationTests "${CMAKE_BINARY_DIR}/raster-validation" --raster)
    add_test(NAME editor_layout_smoke COMMAND DuskGraphIntegrationTests "${CMAKE_BINARY_DIR}/layout-smoke" --layout)
endif()
