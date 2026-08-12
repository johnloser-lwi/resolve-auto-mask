#include "render/CudaRender.h"

#include <cuda_runtime.h>

#include <vector>

// The composite itself is NOT reimplemented here. Composite.h is MTX_HD
// throughout precisely so the kernel below calls the same RenderPixel the CPU
// path does, and the parity test is checking arithmetic reassociation rather
// than two separately maintained versions of the algorithm.

namespace am {
namespace {

inline bool ok(cudaError_t e) { return e == cudaSuccess; }

/// A device buffer that only ever grows.
///
/// Grow-only because cudaMalloc SYNCHRONISES the device. Reallocating per frame
/// would insert a full pipeline stall into every render, which is a strange way
/// to spend the time the GPU path exists to save.
struct DevBuf {
    void* p = nullptr;
    std::size_t bytes = 0;

    bool ensure(std::size_t need)
    {
        if (need <= bytes) return true;
        if (p) cudaFree(p);
        p = nullptr;
        bytes = 0;
        if (!ok(cudaMalloc(&p, need))) return false;
        bytes = need;
        return true;
    }
    void free()
    {
        if (p) cudaFree(p);
        p = nullptr;
        bytes = 0;
    }
};

// ------------------------------------------------------------------- kernels

/// Order-independent content fingerprint.
///
/// Each sampled pixel is hashed together with its own index and the results are
/// summed atomically, so the answer does not depend on which thread got there
/// first. A sequential FNV chain like the host's would have to be replayed in
/// raster order, which across thousands of threads costs more than re-analysing
/// the frame would.
__global__ void kHash(const float* src, int w, int h, long long strideFloats, int mode,
                      float threshold, unsigned long long* out)
{
    const int x = (blockIdx.x * blockDim.x + threadIdx.x) * 8;
    const int y = (blockIdx.y * blockDim.y + threadIdx.y) * 8;
    if (x >= w || y >= h) return;

    const float* px = src + strideFloats * y + (long long)x * 4;
    const float v = (mode == 0) ? px[3]
                                : (0.2126f * px[0] + 0.7152f * px[1] + 0.0722f * px[2]);

    const float c = fminf(fmaxf(v, 0.0f), 1.0f);
    const unsigned q = (unsigned)(c * 15.0f) + (v > threshold ? 16u : 0u);

    // Mix the value with the position so that moving content changes the sum
    // even when the same set of values is present.
    unsigned long long k = (unsigned long long)(y) * 73856093ull ^
                           (unsigned long long)(x) * 19349663ull;
    k = (k + q) * 0x9E3779B97F4A7C15ull;
    k ^= k >> 29;
    k *= 0xBF58476D1CE4E5B9ull;
    k ^= k >> 32;

    atomicAdd(out, k);
}

/// Label image + per-label flags -> a 0/1 byte stencil.
__global__ void kStencil(const int* labels, const unsigned char* selected, unsigned char* out,
                         int n, int labelCount)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    const int l = labels[i];
    out[i] = (l > 0 && l <= labelCount) ? selected[l] : 0;
}

__global__ void kInvertBytes(unsigned char* p, int n)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) p[i] = (unsigned char)(1 - p[i]);
}

/// One thread per row: two sweeps carrying the nearest set pixel behind and
/// ahead, exactly as the CPU version does, so the result is bit-identical
/// rather than merely similar. Sequential along the row and parallel across
/// rows, which is the only decomposition a running-sum sweep allows.
__global__ void kDilateRows(const unsigned char* src, unsigned char* dst, int w, int h, int r)
{
    const int y = blockIdx.x * blockDim.x + threadIdx.x;
    if (y >= h) return;

    const unsigned char* s = src + (long long)y * w;
    unsigned char* d = dst + (long long)y * w;

    const int kFar = 1 << 28;
    int last = -kFar;
    for (int i = 0; i < w; ++i)
    {
        if (s[i]) last = i;
        d[i] = (unsigned char)((i - last) <= r);  // provisional: behind only
    }
    int ahead = kFar;
    for (int i = w - 1; i >= 0; --i)
    {
        if (s[i]) ahead = i;
        if ((ahead - i) <= r) d[i] = 1;
    }
}

__global__ void kBoxRows(const float* src, float* dst, int w, int h, int r)
{
    const int y = blockIdx.x * blockDim.x + threadIdx.x;
    if (y >= h) return;

    const float* s = src + (long long)y * w;
    float* d = dst + (long long)y * w;

    const float inv = 1.0f / (float)(2 * r + 1);
    float sum = s[0] * (float)(r + 1);
    for (int i = 1; i <= r; ++i) sum += s[min(i, w - 1)];

    for (int i = 0; i < w; ++i)
    {
        d[i] = sum * inv;
        sum += s[min(i + r + 1, w - 1)] - s[max(i - r, 0)];
    }
}

template <class T>
__global__ void kTranspose(const T* src, T* dst, int w, int h)
{
    // 32x32 tiles with a padded column, so the write side does not land every
    // element of a tile in the same shared-memory bank.
    __shared__ T tile[32][33];

    const int x = blockIdx.x * 32 + threadIdx.x;
    const int y = blockIdx.y * 32 + threadIdx.y;
    if (x < w && y < h) tile[threadIdx.y][threadIdx.x] = src[(long long)y * w + x];
    __syncthreads();

    const int tx = blockIdx.y * 32 + threadIdx.x;
    const int ty = blockIdx.x * 32 + threadIdx.y;
    if (tx < h && ty < w) dst[(long long)ty * h + tx] = tile[threadIdx.x][threadIdx.y];
}

__global__ void kBytesToFloat(const unsigned char* src, float* dst, int n)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) dst[i] = (float)src[i];
}

__global__ void kInvertFloat(float* p, int n)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) p[i] = 1.0f - p[i];
}

__global__ void kComposite(const float* src, float* dst, const float* mask, int w, int h,
                           long long srcStride, long long dstStride, CompositeParams params)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;

    const ImageView sv{src, w, h, (std::ptrdiff_t)srcStride};
    const PlaneView pv{mask, w, h};
    RenderPixel(sv, pv, params, x, y, dst + dstStride * y + (long long)x * 4);
}

__global__ void kCompositeLayered(const float* src, float* dst, const float* mask,
                                  const float* layerColour, const float* layerCoverage, int w,
                                  int h, int lx0, int ly0, int lw, int lh, long long srcStride,
                                  long long dstStride, CompositeParams params)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h) return;

    const ImageView sv{src, w, h, (std::ptrdiff_t)srcStride};
    const PlaneView pv{mask, w, h};
    const ImageView lc{layerColour, lw, lh, (std::ptrdiff_t)lw * 4};
    const PlaneView la{layerCoverage, lw, lh};
    RenderPixelLayered(sv, pv, lc, la, lx0, ly0, params, x, y,
                       dst + dstStride * y + (long long)x * 4);
}

}  // namespace

// ------------------------------------------------------------------- context

struct CudaContext {
    DevBuf labels;    ///< int32 per pixel
    DevBuf selected;  ///< uint8 per label
    DevBuf mask;      ///< float per pixel, the finished mask
    DevBuf binA, binB;
    DevBuf fltA;
    DevBuf hash;
    DevBuf layerColour;    ///< float4 per pixel, arranged space
    DevBuf layerCoverage;  ///< float per pixel
    bool hasLayer = false;
    int layerX0 = 0, layerY0 = 0, layerW = 0, layerH = 0;
    int labelCount = 0;
    int width = 0, height = 0;
};

bool CudaAvailable()
{
    static const bool available = [] {
        int n = 0;
        return ok(cudaGetDeviceCount(&n)) && n > 0;
    }();
    return available;
}

CudaContext* CudaCreate() { return CudaAvailable() ? new CudaContext() : nullptr; }

void CudaDestroy(CudaContext* ctx)
{
    if (!ctx) return;
    ctx->labels.free();
    ctx->selected.free();
    ctx->mask.free();
    ctx->binA.free();
    ctx->binB.free();
    ctx->fltA.free();
    ctx->hash.free();
    ctx->layerColour.free();
    ctx->layerCoverage.free();
    delete ctx;
}

bool CudaUploadLayer(CudaContext* ctx, const float* colour, const float* coverage, int originX,
                     int originY, int width, int height, void* stream)
{
    if (!ctx || !colour || !coverage) return false;
    const std::size_t n = std::size_t(width) * height;
    if (!ctx->layerColour.ensure(n * 4 * sizeof(float)) ||
        !ctx->layerCoverage.ensure(n * sizeof(float)))
        return false;

    cudaStream_t s = static_cast<cudaStream_t>(stream);
    if (!ok(cudaMemcpyAsync(ctx->layerColour.p, colour, n * 4 * sizeof(float),
                            cudaMemcpyHostToDevice, s)))
        return false;
    if (!ok(cudaMemcpyAsync(ctx->layerCoverage.p, coverage, n * sizeof(float),
                            cudaMemcpyHostToDevice, s)))
        return false;
    ctx->layerX0 = originX;
    ctx->layerY0 = originY;
    ctx->layerW = width;
    ctx->layerH = height;
    ctx->hasLayer = true;
    return true;
}

void CudaClearLayer(CudaContext* ctx)
{
    if (ctx) ctx->hasLayer = false;
}

bool CudaRenderLayered(CudaContext* ctx, const float* devSrc, float* devDst, int width,
                       int height, std::ptrdiff_t srcStrideFloats,
                       std::ptrdiff_t dstStrideFloats, const CompositeParams& params,
                       void* stream)
{
    if (!ctx || !devSrc || !devDst || !ctx->hasLayer) return false;

    cudaStream_t s = static_cast<cudaStream_t>(stream);
    const dim3 block(16, 16);
    const dim3 grid((width + block.x - 1) / block.x, (height + block.y - 1) / block.y);

    kCompositeLayered<<<grid, block, 0, s>>>(
        devSrc, devDst, static_cast<const float*>(ctx->mask.p),
        static_cast<const float*>(ctx->layerColour.p),
        static_cast<const float*>(ctx->layerCoverage.p), width, height, ctx->layerX0,
        ctx->layerY0, ctx->layerW, ctx->layerH, (long long)srcStrideFloats,
        (long long)dstStrideFloats, params);

    if (!ok(cudaGetLastError())) return false;
    if (!s) return ok(cudaDeviceSynchronize());
    return true;
}

std::uint64_t CudaHashSource(CudaContext* ctx, const float* devSrc, int width, int height,
                             std::ptrdiff_t rowStrideFloats, int segMode, float threshold,
                             void* stream)
{
    if (!ctx || !devSrc || width <= 0 || height <= 0) return 0;
    if (!ctx->hash.ensure(sizeof(unsigned long long))) return 0;

    cudaStream_t s = static_cast<cudaStream_t>(stream);
    if (!ok(cudaMemsetAsync(ctx->hash.p, 0, sizeof(unsigned long long), s))) return 0;

    const dim3 block(16, 16);
    const dim3 grid(((width + 7) / 8 + block.x - 1) / block.x,
                    ((height + 7) / 8 + block.y - 1) / block.y);
    kHash<<<grid, block, 0, s>>>(devSrc, width, height, (long long)rowStrideFloats, segMode,
                                 threshold, static_cast<unsigned long long*>(ctx->hash.p));

    unsigned long long out = 0;
    if (!ok(cudaMemcpyAsync(&out, ctx->hash.p, sizeof(out), cudaMemcpyDeviceToHost, s))) return 0;
    cudaStreamSynchronize(s);  // eight bytes; this is the whole point of hashing on device

    // Fold in the dimensions, so a resize invalidates even if the content sum
    // happened to survive it.
    out ^= (unsigned long long)width * 0x100000001B3ull;
    out ^= (unsigned long long)height * 0x9E3779B97F4A7C15ull;
    return out;
}

bool CudaDownload(CudaContext* ctx, const float* devSrc, int width, int height,
                  std::ptrdiff_t rowStrideFloats, float* hostDst, void* stream)
{
    if (!ctx || !devSrc || !hostDst) return false;
    cudaStream_t s = static_cast<cudaStream_t>(stream);

    // Row by row, because the host copy is tightly packed and the device rows
    // may not be. cudaMemcpy2D handles a positive pitch; a negative stride
    // (a top-down view of a bottom-up image) is expressed by pointing at the
    // last row and walking backwards, which 2D copies cannot do -- so the
    // caller passes the natural bottom-up view and the flip stays on the host.
    const std::size_t rowBytes = std::size_t(width) * 4 * sizeof(float);
    if (rowStrideFloats < 0) return false;

    if (!ok(cudaMemcpy2DAsync(hostDst, rowBytes, devSrc, std::size_t(rowStrideFloats) * sizeof(float),
                              rowBytes, std::size_t(height), cudaMemcpyDeviceToHost, s)))
        return false;
    return ok(cudaStreamSynchronize(s));
}

bool CudaUploadLabels(CudaContext* ctx, const std::int32_t* labels, int width, int height,
                      void* stream)
{
    if (!ctx || !labels) return false;
    const std::size_t n = std::size_t(width) * height;
    if (!ctx->labels.ensure(n * sizeof(std::int32_t))) return false;

    cudaStream_t s = static_cast<cudaStream_t>(stream);
    ctx->width = width;
    ctx->height = height;
    return ok(cudaMemcpyAsync(ctx->labels.p, labels, n * sizeof(std::int32_t),
                              cudaMemcpyHostToDevice, s));
}

bool CudaUploadSelection(CudaContext* ctx, const std::uint8_t* selected, int labelCount,
                         void* stream)
{
    if (!ctx || !selected || labelCount < 0) return false;
    const std::size_t n = std::size_t(labelCount) + 1;
    if (!ctx->selected.ensure(n)) return false;

    cudaStream_t s = static_cast<cudaStream_t>(stream);
    ctx->labelCount = labelCount;
    return ok(cudaMemcpyAsync(ctx->selected.p, selected, n, cudaMemcpyHostToDevice, s));
}

bool CudaBuildMask(CudaContext* ctx, int width, int height, int expand, float feather,
                   bool invert, void* stream)
{
    if (!ctx || width <= 0 || height <= 0) return false;
    const int n = width * height;
    const std::size_t nBytes = std::size_t(n);

    if (!ctx->binA.ensure(nBytes) || !ctx->binB.ensure(nBytes)) return false;
    if (!ctx->mask.ensure(nBytes * sizeof(float))) return false;
    if (!ctx->fltA.ensure(nBytes * sizeof(float))) return false;

    cudaStream_t s = static_cast<cudaStream_t>(stream);
    auto* binA = static_cast<unsigned char*>(ctx->binA.p);
    auto* binB = static_cast<unsigned char*>(ctx->binB.p);
    auto* maskF = static_cast<float*>(ctx->mask.p);
    auto* fltA = static_cast<float*>(ctx->fltA.p);

    const int lin = 256;
    const int linGrid = (n + lin - 1) / lin;

    kStencil<<<linGrid, lin, 0, s>>>(static_cast<const int*>(ctx->labels.p),
                                     static_cast<const unsigned char*>(ctx->selected.p), binA, n,
                                     ctx->labelCount);

    // Expand / contract. Erosion is dilation of the complement, complemented --
    // the same identity the CPU path uses, so the two cannot disagree at the
    // edges.
    const int r = expand < 0 ? -expand : expand;
    if (r > 0)
    {
        if (expand < 0) kInvertBytes<<<linGrid, lin, 0, s>>>(binA, n);

        const int rowsBlock = 128;
        kDilateRows<<<(height + rowsBlock - 1) / rowsBlock, rowsBlock, 0, s>>>(binA, binB, width,
                                                                               height, r);
        kTranspose<unsigned char>
            <<<dim3((width + 31) / 32, (height + 31) / 32), dim3(32, 32), 0, s>>>(binB, binA,
                                                                                  width, height);
        kDilateRows<<<(width + rowsBlock - 1) / rowsBlock, rowsBlock, 0, s>>>(binA, binB, height,
                                                                              width, r);
        kTranspose<unsigned char>
            <<<dim3((height + 31) / 32, (width + 31) / 32), dim3(32, 32), 0, s>>>(binB, binA,
                                                                                  height, width);

        if (expand < 0) kInvertBytes<<<linGrid, lin, 0, s>>>(binA, n);
    }

    kBytesToFloat<<<linGrid, lin, 0, s>>>(binA, maskF, n);

    if (feather > 0.0f)
    {
        const float sigma = feather * 0.5f;
        int b = (int)(sqrtf(12.0f * sigma * sigma / 3.0f + 1.0f) + 0.5f);
        if (b < 1) b = 1;
        if ((b & 1) == 0) ++b;
        const int br = (b - 1) / 2;
        if (br > 0)
        {
            const int rowsBlock = 128;
            const int hGrid = (height + rowsBlock - 1) / rowsBlock;
            const int wGrid = (width + rowsBlock - 1) / rowsBlock;

            // Three passes horizontally, transpose, three more, transpose back
            // -- the same shape as the CPU version and for the same reason:
            // a strided column walk is a cache miss per sample there and an
            // uncoalesced load per lane here.
            for (int pass = 0; pass < 3; ++pass)
            {
                kBoxRows<<<hGrid, rowsBlock, 0, s>>>(maskF, fltA, width, height, br);
                std::swap(maskF, fltA);
            }
            kTranspose<float><<<dim3((width + 31) / 32, (height + 31) / 32), dim3(32, 32), 0, s>>>(
                maskF, fltA, width, height);
            std::swap(maskF, fltA);
            for (int pass = 0; pass < 3; ++pass)
            {
                kBoxRows<<<wGrid, rowsBlock, 0, s>>>(maskF, fltA, height, width, br);
                std::swap(maskF, fltA);
            }
            kTranspose<float><<<dim3((height + 31) / 32, (width + 31) / 32), dim3(32, 32), 0, s>>>(
                maskF, fltA, height, width);
            std::swap(maskF, fltA);

            // An odd number of swaps would leave the finished mask in the
            // scratch buffer, and the composite reads ctx->mask.
            if (maskF != static_cast<float*>(ctx->mask.p))
            {
                if (!ok(cudaMemcpyAsync(ctx->mask.p, maskF, nBytes * sizeof(float),
                                        cudaMemcpyDeviceToDevice, s)))
                    return false;
            }
        }
    }

    if (invert)
        kInvertFloat<<<linGrid, lin, 0, s>>>(static_cast<float*>(ctx->mask.p), n);

    return ok(cudaGetLastError());
}

bool CudaRender(CudaContext* ctx, const float* devSrc, float* devDst, int width, int height,
                std::ptrdiff_t srcStrideFloats, std::ptrdiff_t dstStrideFloats,
                const CompositeParams& params, void* stream)
{
    if (!ctx || !devSrc || !devDst) return false;

    cudaStream_t s = static_cast<cudaStream_t>(stream);
    const dim3 block(16, 16);
    const dim3 grid((width + block.x - 1) / block.x, (height + block.y - 1) / block.y);

    kComposite<<<grid, block, 0, s>>>(devSrc, devDst, static_cast<const float*>(ctx->mask.p),
                                      width, height, (long long)srcStrideFloats,
                                      (long long)dstStrideFloats, params);

    if (!ok(cudaGetLastError())) return false;

    // Without a stream the host has given us nothing to order against, so the
    // work has to be known finished before the image object is released.
    if (!s) return ok(cudaDeviceSynchronize());
    return true;
}

bool CudaRenderLayeredHostSync(const float* hostSrc, float* hostDst, const std::int32_t* labels,
                               const std::uint8_t* selected, int labelCount, int width,
                               int height, int expand, float feather, bool invert,
                               const float* layerColour, const float* layerCoverage, int layerX0,
                               int layerY0, int layerW, int layerH,
                               const CompositeParams& params)
{
    if (!CudaAvailable()) return false;

    CudaContext* ctx = CudaCreate();
    if (!ctx) return false;

    const std::size_t px = std::size_t(width) * height;
    const std::size_t bytes = px * 4 * sizeof(float);

    DevBuf src, dst;
    bool good = src.ensure(bytes) && dst.ensure(bytes);
    good = good && ok(cudaMemcpy(src.p, hostSrc, bytes, cudaMemcpyHostToDevice));
    good = good && CudaUploadLabels(ctx, labels, width, height, nullptr);
    good = good && CudaUploadSelection(ctx, selected, labelCount, nullptr);
    // The mask is still needed: it is what the hole and the Preview dim read.
    good = good && CudaBuildMask(ctx, width, height, expand, feather, invert, nullptr);
    good = good && CudaUploadLayer(ctx, layerColour, layerCoverage, layerX0, layerY0, layerW,
                                   layerH, nullptr);
    good = good && CudaRenderLayered(ctx, static_cast<const float*>(src.p),
                                     static_cast<float*>(dst.p), width, height,
                                     std::ptrdiff_t(width) * 4, std::ptrdiff_t(width) * 4,
                                     params, nullptr);
    good = good && ok(cudaDeviceSynchronize());
    good = good && ok(cudaMemcpy(hostDst, dst.p, bytes, cudaMemcpyDeviceToHost));

    src.free();
    dst.free();
    CudaDestroy(ctx);
    return good;
}

bool CudaBench(int width, int height, int expand, float feather, bool invert,
               const CompositeParams& params, int iterations, double& outMsPerFrame)
{
    if (!CudaAvailable() || iterations <= 0) return false;

    CudaContext* ctx = CudaCreate();
    if (!ctx) return false;

    const std::size_t px = std::size_t(width) * height;
    DevBuf src, dst;
    std::vector<std::int32_t> labels(px, 1);
    const std::uint8_t selected[2] = {0, 1};

    bool good = src.ensure(px * 4 * sizeof(float)) && dst.ensure(px * 4 * sizeof(float));
    good = good && ok(cudaMemset(src.p, 0, px * 4 * sizeof(float)));
    good = good && CudaUploadLabels(ctx, labels.data(), width, height, nullptr);
    good = good && CudaUploadSelection(ctx, selected, 1, nullptr);

    cudaEvent_t t0, t1;
    good = good && ok(cudaEventCreate(&t0)) && ok(cudaEventCreate(&t1));
    if (!good)
    {
        src.free();
        dst.free();
        CudaDestroy(ctx);
        return false;
    }

    // One warm-up: the first launch pays kernel load and any lazy JIT, which is
    // not a per-frame cost and would skew a short run badly.
    CudaBuildMask(ctx, width, height, expand, feather, invert, nullptr);
    CudaRender(ctx, static_cast<const float*>(src.p), static_cast<float*>(dst.p), width, height,
               std::ptrdiff_t(width) * 4, std::ptrdiff_t(width) * 4, params, nullptr);
    cudaDeviceSynchronize();

    cudaEventRecord(t0);
    for (int i = 0; i < iterations; ++i)
    {
        CudaBuildMask(ctx, width, height, expand, feather, invert, nullptr);
        CudaRender(ctx, static_cast<const float*>(src.p), static_cast<float*>(dst.p), width,
                   height, std::ptrdiff_t(width) * 4, std::ptrdiff_t(width) * 4, params, nullptr);
    }
    cudaEventRecord(t1);
    cudaEventSynchronize(t1);

    float ms = 0.0f;
    cudaEventElapsedTime(&ms, t0, t1);
    outMsPerFrame = double(ms) / double(iterations);

    cudaEventDestroy(t0);
    cudaEventDestroy(t1);
    src.free();
    dst.free();
    CudaDestroy(ctx);
    return true;
}

bool CudaRenderHostSync(const float* hostSrc, float* hostDst, const std::int32_t* labels,
                        const std::uint8_t* selected, int labelCount, int width, int height,
                        int expand, float feather, bool invert, const CompositeParams& params)
{
    if (!CudaAvailable()) return false;

    CudaContext* ctx = CudaCreate();
    if (!ctx) return false;

    const std::size_t px = std::size_t(width) * height;
    const std::size_t bytes = px * 4 * sizeof(float);

    DevBuf src, dst;
    bool good = src.ensure(bytes) && dst.ensure(bytes);
    good = good && ok(cudaMemcpy(src.p, hostSrc, bytes, cudaMemcpyHostToDevice));
    good = good && CudaUploadLabels(ctx, labels, width, height, nullptr);
    good = good && CudaUploadSelection(ctx, selected, labelCount, nullptr);
    good = good && CudaBuildMask(ctx, width, height, expand, feather, invert, nullptr);
    good = good && CudaRender(ctx, static_cast<const float*>(src.p), static_cast<float*>(dst.p),
                              width, height, std::ptrdiff_t(width) * 4,
                              std::ptrdiff_t(width) * 4, params, nullptr);
    good = good && ok(cudaDeviceSynchronize());
    good = good && ok(cudaMemcpy(hostDst, dst.p, bytes, cudaMemcpyDeviceToHost));

    src.free();
    dst.free();
    CudaDestroy(ctx);
    return good;
}

}  // namespace am
