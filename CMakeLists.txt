cmake_minimum_required(VERSION 3.24)

# CUDA is deliberately NOT listed here, and is OFF by default (see below).
# Naming it in project(LANGUAGES ...) enables the language immediately, and
# enabling CUDA installs a default CMAKE_CUDA_ARCHITECTURES -- after which any
# `if(NOT DEFINED ...)` guard silently never fires and the plugin ships PTX-only
# code the driver must JIT at load. Architectures must be chosen BEFORE
# enable_language(CUDA).
project(AutoMask VERSION 0.1.0 LANGUAGES CXX)

include(CMakeDependentOption)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32 AND NOT APPLE)
    message(FATAL_ERROR "This project targets Windows and macOS.")
endif()

# An OFX bundle names its payload directory after the architecture, and the
# host looks in exactly one place. Get this wrong and the plugin is simply
# never found, with no error from anywhere.
if(WIN32)
    set(OFX_ARCH_DIR "Win64")
else()
    # macOS uses a single MacOS directory for every slice, universal or not --
    # NOT MacOS-x86-64, which is a Linux-style name that Resolve ignores.
    set(OFX_ARCH_DIR "MacOS")
endif()

if(MSVC)
    # Static CRT. An OFX bundle is loaded by a host that will not have our
    # redistributables installed, and a missing VCRUNTIME dll fails as a
    # *silent no-load*: Resolve simply never lists the plugin, with no error
    # anywhere.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(APPLE)
    # Universal by default. Resolve runs native arm64 on Apple Silicon and
    # x86_64 on Intel, and a single-slice bundle is another way to be silently
    # not listed -- the host loads what matches its own process and says
    # nothing when nothing does.
    option(AUTOMASK_UNIVERSAL "Build a universal arm64 + x86_64 binary" ON)
    if(AUTOMASK_UNIVERSAL AND NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
    endif()
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        # Below what any Resolve version needing this plugin runs on, so the
        # deployment target is never the thing that rejects a machine.
        set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "" FORCE)
    endif()

    # Visibility is deliberately left at the default. The OFX support library
    # marks its entry points with __attribute__((visibility("default"))), so
    # hiding everything else would work -- but a plugin whose OfxGetPlugin is
    # not exported fails as a silent no-load, and that is not a failure worth
    # risking to shrink a symbol table.
endif()

# Single-config generators leave CMAKE_BUILD_TYPE empty, which collapses the
# per-config bundle path below and produces an unoptimised plugin that is slow
# for no visible reason.
get_property(_isMultiConfig GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _isMultiConfig AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# --- CUDA --------------------------------------------------------------------
# ON wherever a toolkit exists, because the CPU cannot reach real time and the
# reason is bandwidth rather than arithmetic: at UHD, reading and writing one
# RGBA float frame -- no feather, no transform, nothing -- measures 14.8 ms on
# 24 threads against a 60 fps budget of 16.7 ms. The same traffic on the GPU is
# about 0.07 ms. Resolve's pipeline is GPU-resident too, so declining CUDA also
# forces a download and upload of every frame.
#
# Labelling stays on the CPU regardless: it is serial, and cached per content
# change. CudaHashSource is what stops that costing a transfer per frame.
#
# No CUDA on Apple, at any version -- the toolkit was dropped after 10.13 and
# Apple Silicon never had it. macOS therefore runs the CPU path, and the GPU
# story there is Metal, which is NOT written (see the README).
cmake_dependent_option(AUTOMASK_CUDA "Build the CUDA render path" ON "NOT APPLE" OFF)
set(AUTOMASK_HAVE_CUDA OFF)
if(AUTOMASK_CUDA)
    # Real SASS for Turing/Ampere/Ada/Blackwell plus virtual PTX so
    # architectures newer than this toolkit still run. CUDA 13 dropped
    # everything below Turing, so there is no point listing 60/70.
    set(CMAKE_CUDA_ARCHITECTURES 75 86 89 120 120-virtual)
    include(CheckLanguage)
    check_language(CUDA)
    if(CMAKE_CUDA_COMPILER)
        enable_language(CUDA)
        set(CMAKE_CUDA_STANDARD 17)
        set(CMAKE_CUDA_STANDARD_REQUIRED ON)
        set(AUTOMASK_HAVE_CUDA ON)
        message(STATUS "CUDA enabled, architectures: ${CMAKE_CUDA_ARCHITECTURES}")
    else()
        message(WARNING "AUTOMASK_CUDA is ON but no CUDA compiler was found.")
    endif()
endif()

set(OFX_ROOT     "${CMAKE_SOURCE_DIR}/third_party/openfx")
set(OFX_INCLUDE  "${OFX_ROOT}/OpenFX-1.4/include")
set(OFX_SUPPORT  "${OFX_ROOT}/Support")

# --- OFX C++ support library -------------------------------------------------
# Vendored from C:\ProgramData\Blackmagic Design\DaVinci Resolve\Support\Developer
# (ProgramData, not Program Files). Blackmagic's copy is used rather than upstream
# OpenFX specifically because it BINDS OFX::Private::gDrawSuite -- upstream ships
# ofxDrawSuite.h but never fetches it, which forces a plugin to wrap the interact
# main entry by hand just to steal the draw context out of the draw action's
# inArgs. Vendoring this copy deletes that whole shim.
#
# ofxsHWNDInteract.cpp is deliberately excluded: it references symbols
# (gHWNDInteractSuite, kOfxHWndInteract*) that are declared nowhere in the SDK, so
# it does not compile. Blackmagic's own Makefile omits it too. It is a vestigial
# Foundry extension, not a Resolve feature -- do not try to revive it.
add_library(ofxsupport STATIC
    "${OFX_SUPPORT}/Library/ofxsCore.cpp"
    "${OFX_SUPPORT}/Library/ofxsImageEffect.cpp"
    "${OFX_SUPPORT}/Library/ofxsInteract.cpp"
    "${OFX_SUPPORT}/Library/ofxsLog.cpp"
    "${OFX_SUPPORT}/Library/ofxsMultiThread.cpp"
    "${OFX_SUPPORT}/Library/ofxsParams.cpp"
    "${OFX_SUPPORT}/Library/ofxsProperty.cpp"
    "${OFX_SUPPORT}/Library/ofxsPropertyValidation.cpp"
)
target_include_directories(ofxsupport PUBLIC "${OFX_INCLUDE}" "${OFX_SUPPORT}/include")
target_compile_definitions(ofxsupport PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

# --- CUDA render path ----------------------------------------------------------
# Its own library so the plugin and the parity test link the exact same compiled
# kernel rather than two separate builds of it. Two builds of "the same" kernel
# is precisely the situation the parity test exists to rule out.
if(AUTOMASK_HAVE_CUDA)
    add_library(amcuda STATIC src/render/CudaKernel.cu)
    target_include_directories(amcuda PUBLIC src)
    set_target_properties(amcuda PROPERTIES
        CUDA_SEPARABLE_COMPILATION OFF
        CUDA_RUNTIME_LIBRARY Static
        POSITION_INDEPENDENT_CODE ON
    )
    # Deliberately NOT --use_fast_math. The whole point of the shared header is
    # that the CPU and GPU agree; fast-math lets nvcc substitute lower-precision
    # intrinsics on one side of that pair only, and the parity test would start
    # failing for a reason that has nothing to do with the code.
endif()

# --- The plugin --------------------------------------------------------------
# src/core includes no OFX header, so the offline harness links the identical
# code Resolve runs. Keep it that way: the moment core needs ofxsImageEffect.h,
# every tuning pass costs a Resolve relaunch instead of a second.
add_library(AutoMask MODULE
    src/AutoMaskPlugin.cpp
    src/HostProbe.cpp
    src/core/Segmentation.cpp
    src/interact/Overlay.cpp
)
target_include_directories(AutoMask PRIVATE src)
target_link_libraries(AutoMask PRIVATE ofxsupport)

if(AUTOMASK_HAVE_CUDA)
    target_link_libraries(AutoMask PRIVATE amcuda)
    target_compile_definitions(AutoMask PRIVATE AUTOMASK_HAVE_CUDA)
endif()

# OFX plugins are DLLs with a .ofx extension and a single exported entry point.
set_target_properties(AutoMask PROPERTIES
    PREFIX ""
    SUFFIX ".ofx"
    OUTPUT_NAME "AutoMask"
)

# --- Offline harness ---------------------------------------------------------
# Segments a PNG and writes the mask straight back out with no host in the loop.
# This is where Threshold, Bridge Gaps and Min Element Size get tuned, because
# here a guess costs a second instead of a Resolve relaunch.
enable_testing()
add_executable(masktest tools/masktest.cpp src/core/Segmentation.cpp)
target_include_directories(masktest PRIVATE src third_party/stb)

set(TESTDATA "${CMAKE_SOURCE_DIR}/tests/data")
set(SCRATCH  "${CMAKE_BINARY_DIR}/testout")
file(MAKE_DIRECTORY "${SCRATCH}")

# Three shapes on transparent black must come back as three elements, not two
# (the antialiased fringes bridging) and not thirty (despeckle asleep).
add_test(NAME segment_alpha
         COMMAND masktest "${TESTDATA}/three_shapes.png" --stats --expect 3)

# The Luminance path with Invert Source, which is the case the alpha-only hash
# in the original would have served stale forever.
add_test(NAME segment_luma
         COMMAND masktest "${TESTDATA}/dark_on_light.png"
                 --mode luma --threshold 0.5 --invert-source --stats --expect 2)

# The regression test for the recombination algebra in Composite.h.
#
# A composite with the transform at defaults must return the source untouched,
# feather and all. The obvious formulation -- Ew + B*(1-Aw) -- passes every
# visual inspection with a hard mask and is wrong by a quarter of a stop along
# every soft edge, which is why this runs with a feather rather than without.
add_test(NAME identity_composite
         COMMAND masktest "${TESTDATA}/three_shapes.png" "${SCRATCH}/identity.png"
                 --select-all --output composite --feather 8 --identity-check)

# Auto Center, on the triangle -- deliberately, because its bounding-box centre
# and its centroid are nowhere near each other, so a version that centred the
# wrong one would pass on the circle and fail here.
add_test(NAME auto_center
         COMMAND masktest "${TESTDATA}/three_shapes.png" "${SCRATCH}/centered.png"
                 --select "0.34,0.79" --output cutoutpm --auto-center --center-check)

# And again with a scale and a rotation in force. Solving for the translation
# without accounting for those is right only at scale 1 / rotation 0, which is
# exactly the case the simple test above cannot distinguish.
# Arrange ordering. three_shapes.png is circle(1) rect(2) triangle(3) in raster
# order, so every sort below reorders it differently -- a shape set where two
# orders agreed would not be testing much.
add_test(NAME arrange_order_lr
         COMMAND masktest "${TESTDATA}/three_shapes.png" --select-all
                 --order lr --step 0.05,0 --expect-order "1,3,2")
add_test(NAME arrange_order_largest
         COMMAND masktest "${TESTDATA}/three_shapes.png" --select-all
                 --order largest --step 0.05,0 --expect-order "2,1,3")
add_test(NAME arrange_order_reverse
         COMMAND masktest "${TESTDATA}/three_shapes.png" --select-all
                 --order lr --reverse --step 0.05,0 --expect-order "2,3,1")

# Settings that mean "no arrangement" must reach the SINGLE-PASS path, not the
# layered one. Not merely for speed: only the single-pass path has the exact
# identity guarantee, because per-element feathering makes the element coverages
# overlap slightly and their sum is no longer exactly the combined mask.
add_test(NAME arrange_identity
         COMMAND masktest "${TESTDATA}/three_shapes.png" "${SCRATCH}/arrange_identity.png"
                 --select-all --output composite --feather 8
                 --spread 1.0,1.0 --identity-check)

# Align and Step COMPOSE. A left-aligned column is the case an exclusive mode
# could not express, and the one that motivated dropping it.
add_test(NAME arrange_align_and_step
         COMMAND masktest "${TESTDATA}/three_shapes.png" "${SCRATCH}/align_step.png"
                 --select-all --output preview --align-h left --step 0,-0.15
                 --order tb --expect-order "1,2,3")

# Clipping order. Arrange runs BEFORE the global transform, so an element it
# pushes outside the frame must survive to be scaled back in afterwards.
#
# A Step of 0.4 puts two of the three shapes well off the bottom; a global scale
# of 0.4 brings all three back. Unarranged the frame holds 14600 opaque pixels,
# so at 0.4 scale (0.16 area) roughly 2340 should survive. When the element
# layer was sized to the frame, the off-frame shapes were destroyed before the
# transform ever ran and the count collapsed. The picture looked entirely
# plausible either way, which is why this is a count and not an eyeball.
add_test(NAME arrange_survives_transform
         COMMAND masktest "${TESTDATA}/three_shapes.png" "${SCRATCH}/offframe.png"
                 --select-all --output cutoutpm --order lr --step 0,-0.4 --scale 0.4
                 --expect-alpha-min 2200)

# CPU/GPU parity. Skips itself, rather than failing, on a machine with no GPU.
if(AUTOMASK_HAVE_CUDA)
    add_executable(cudatest tools/cudatest.cpp src/core/Segmentation.cpp)
    target_include_directories(cudatest PRIVATE src)
    target_link_libraries(cudatest PRIVATE amcuda)
    add_test(NAME cuda_parity COMMAND cudatest)
endif()

add_test(NAME auto_center_transformed
         COMMAND masktest "${TESTDATA}/three_shapes.png" "${SCRATCH}/centered_xf.png"
                 --select "0.34,0.79" --output cutoutpm --scale 1.6,0.7 --rot 35
                 --anchor 0.2,0.2 --auto-center --center-check)

# --- Assemble the .ofx.bundle ------------------------------------------------
# Per-config path: with a multi-config generator a single shared bundle dir means
# a Debug build silently overwrites the Release one, and the install scripts
# cannot tell which config they are actually shipping.
set(BUNDLE_DIR "${CMAKE_BINARY_DIR}/$<CONFIG>/AutoMask.ofx.bundle")
add_custom_command(TARGET AutoMask POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${BUNDLE_DIR}/Contents/${OFX_ARCH_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:AutoMask>" "${BUNDLE_DIR}/Contents/${OFX_ARCH_DIR}/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/packaging/Info.plist" "${BUNDLE_DIR}/Contents/"
    COMMENT "Assembling AutoMask.ofx.bundle"
)
