#pragma once

// The CUDA path, behind an interface with no CUDA headers in it, so the plugin
// translation unit compiles with MSVC and only the .cu goes through nvcc.
//
// Why there is a GPU path at all, since the first version deliberately had
// none: the CPU is memory-bound long before it is compute-bound. Measured at
// UHD on 24 threads, reading and writing one RGBA float frame -- no feather, no
// transform, nothing -- costs 14.8 ms. A 60 fps budget is 16.7 ms, so a CPU
// implementation is at the wall before it does any work. The same 33 MB each
// way is about 0.07 ms of GPU bandwidth.
//
// On top of that, Resolve's pipeline is GPU-resident. A plugin that declines
// CUDA is not merely slower: it forces the host to copy every frame down to
// system memory and back up again, a cost that does not even appear in the CPU
// measurements above.
//
// What stays on the CPU is the connected-component labelling. It is inherently
// serial, and it is cached per content change, so it runs once per edit rather
// than once per frame. The trick that keeps that from costing a download every
// frame is CudaHashSource: the fingerprint is computed on the device and read
// back as eight bytes, so a cache hit transfers almost nothing and the full
// frame comes down only when the content has actually changed.

#include <cstddef>
#include <cstdint>

#include "core/Composite.h"

namespace am {

/// True when a usable CUDA device is present. Cached after the first call.
///
/// Checked before declaring CUDA support to the host, because claiming it on a
/// machine with no device would have the host hand over device pointers that
/// cannot be serviced -- and by then there is no way back to the CPU loop,
/// since the pixel addresses are already device memory.
bool CudaAvailable();

/// Per-instance device state: the label image, the selection lookup, the mask
/// and its scratch buffers, all kept resident between frames.
struct CudaContext;

CudaContext* CudaCreate();
void CudaDestroy(CudaContext* ctx);

/// Content fingerprint, computed on the device.
///
/// Deliberately NOT the same function as the host's HashSource. FNV-1a is a
/// sequential chain and reproducing its exact order across thousands of threads
/// would cost more than it saves. This uses an order-independent combine
/// instead, which is all a cache key needs. The two are never compared against
/// each other: a given instance runs on one path or the other, and if a host
/// ever switched mid-session the worst case is one extra re-analysis.
std::uint64_t CudaHashSource(CudaContext* ctx, const float* devSrc, int width, int height,
                             std::ptrdiff_t rowStrideFloats, int segMode, float threshold,
                             void* stream);

/// Device -> host, so the CPU can label the frame. Only called when the hash
/// says the content changed.
bool CudaDownload(CudaContext* ctx, const float* devSrc, int width, int height,
                  std::ptrdiff_t rowStrideFloats, float* hostDst, void* stream);

/// Upload a freshly computed label image. Kept resident afterwards.
bool CudaUploadLabels(CudaContext* ctx, const std::int32_t* labels, int width, int height,
                      void* stream);

/// Upload the per-label selected flags. Cheap -- one byte per element -- so it
/// happens whenever the selection changes, without a separate cache.
bool CudaUploadSelection(CudaContext* ctx, const std::uint8_t* selected, int labelCount,
                         void* stream);

/// Build the mask on the device from the resident labels and selection.
bool CudaBuildMask(CudaContext* ctx, int width, int height, int expand, float feather,
                   bool invert, void* stream);

/// Composite into @p devDst using the resident mask.
bool CudaRender(CudaContext* ctx, const float* devSrc, float* devDst, int width, int height,
                std::ptrdiff_t srcStrideFloats, std::ptrdiff_t dstStrideFloats,
                const CompositeParams& params, void* stream);

/// Upload an arranged element layer, built on the host.
///
/// The layer is built on the CPU rather than scattered on the device, and that
/// is a considered trade rather than laziness. Scattering means one kernel per
/// element with a per-element feathered tile, and the elements have to be
/// composited in order because "over" does not commute -- so the parallelism is
/// only within each element anyway. Meanwhile the layer is built in arranged
/// space, so it survives every change to Position, Scale and Rotation, and the
/// upload happens when the SELECTION or the arrangement changes rather than per
/// frame.
bool CudaUploadLayer(CudaContext* ctx, const float* colour, const float* coverage, int originX,
                     int originY, int width, int height, void* stream);

/// Drop any resident layer, so the next render uses the single-pass path.
void CudaClearLayer(CudaContext* ctx);

/// Composite using the resident layer instead of warping the source directly.
bool CudaRenderLayered(CudaContext* ctx, const float* devSrc, float* devDst, int width,
                       int height, std::ptrdiff_t srcStrideFloats,
                       std::ptrdiff_t dstStrideFloats, const CompositeParams& params,
                       void* stream);

/// Synchronous entry point for the parity test, running the whole chain on
/// host buffers. Exists so the test links the IDENTICAL compiled kernel the
/// plugin does, rather than a second build of "the same" code -- which is
/// precisely the situation a parity test is there to rule out.
/// Times the per-frame device work -- mask build plus composite -- with the
/// labels already resident, which is the steady state the plugin runs in.
///
/// Deliberately excludes the host transfers: on the CUDA path Resolve hands
/// over device pointers and there are none, and including them would measure a
/// cost the plugin does not pay.
bool CudaBench(int width, int height, int expand, float feather, bool invert,
               const CompositeParams& params, int iterations, double& outMsPerFrame);

bool CudaRenderHostSync(const float* hostSrc, float* hostDst, const std::int32_t* labels,
                        const std::uint8_t* selected, int labelCount, int width, int height,
                        int expand, float feather, bool invert, const CompositeParams& params);

/// The same, through the layered path, for an element layer built on the host.
bool CudaRenderLayeredHostSync(const float* hostSrc, float* hostDst, const std::int32_t* labels,
                               const std::uint8_t* selected, int labelCount, int width,
                               int height, int expand, float feather, bool invert,
                               const float* layerColour, const float* layerCoverage, int layerX0,
                               int layerY0, int layerW, int layerH,
                               const CompositeParams& params);

}  // namespace am
