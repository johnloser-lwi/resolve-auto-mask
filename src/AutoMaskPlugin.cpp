#include "AutoMaskPlugin.h"

#include "ofxsMultiThread.h"
#include "ofxsProcessing.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "EditBlock.h"
#include "HostProbe.h"
#include "ParamNames.h"
#include "ParamRegistry.h"
#include "core/AutoCenter.h"
#include "core/Composite.h"
#include "core/MaskBuild.h"
#include "core/Segmentation.h"
#include "core/Selection.h"
#include "interact/Overlay.h"

namespace {

using namespace am;

// ---------------------------------------------------------------- boundaries

// OFX images are bottom-up: y == 0 is the bottom row. Everything under core is
// top-down. Converting here, at the single boundary, is what keeps the harness
// and the plugin agreeing about which way is up -- and it costs nothing,
// because the flip is a negative row stride rather than a pass over the pixels.
ImageView MakeSrcView(OFX::Image* img, const OfxRectI& region)
{
    ImageView v;
    if (!img) return v;
    const void* base = img->getPixelAddress(region.x1, region.y2 - 1);  // topmost row
    if (!base) return v;
    v.data = static_cast<const float*>(base);
    v.width = region.x2 - region.x1;
    v.height = region.y2 - region.y1;
    v.rowStride = -std::ptrdiff_t(img->getRowBytes()) / std::ptrdiff_t(sizeof(float));
    return v;
}

MutableImageView MakeDstView(OFX::Image* img, const OfxRectI& region)
{
    MutableImageView v;
    if (!img) return v;
    void* base = img->getPixelAddress(region.x1, region.y2 - 1);
    if (!base) return v;
    v.data = static_cast<float*>(base);
    v.width = region.x2 - region.x1;
    v.height = region.y2 - region.y1;
    v.rowStride = -std::ptrdiff_t(img->getRowBytes()) / std::ptrdiff_t(sizeof(float));
    return v;
}

// --------------------------------------------------------- parameter reading

double DoubleAt(OFX::DoubleParam* p, double t)
{
    double v = 0.0;
    if (p) p->getValueAtTime(t, v);
    return v;
}

void Double2DAt(OFX::Double2DParam* p, double t, double& x, double& y)
{
    x = y = 0.0;
    if (p) p->getValueAtTime(t, x, y);
}

int IntAt(OFX::IntParam* p, double t)
{
    int v = 0;
    if (p) p->getValueAtTime(t, v);
    return v;
}

bool BoolAt(OFX::BooleanParam* p, double t)
{
    bool v = false;
    if (p) p->getValueAtTime(t, v);
    return v;
}

int ChoiceAt(OFX::ChoiceParam* p, double t)
{
    int v = 0;
    if (p) p->getValueAtTime(t, v);
    return v;
}

std::string StringOf(OFX::StringParam* p)
{
    std::string s;
    if (p) p->getValue(s);
    return s;
}

// ------------------------------------------------------------------ renderer

/// Splits the render window into horizontal bands.
///
/// The analysis is not parallel and does not need to be -- it is cached per
/// content change, so it runs once per edit rather than once per frame. This
/// covers the part that does run every frame: one RenderPixel per output pixel.
class BandRenderer : public OFX::MultiThread::Processor
{
public:
    BandRenderer(const ImageView& src, const MutableImageView& dst, const PlaneView& mask,
                 const CompositeParams& cp, const RectI& window, const ElementLayer* layer)
        : _src(src), _dst(dst), _mask(mask), _cp(cp), _window(window), _layer(layer)
    {
        if (_layer)
        {
            _layerColour = ImageView{_layer->colour.data(), _layer->width, _layer->height,
                                     std::ptrdiff_t(_layer->width) * 4};
            _layerCoverage = PlaneView{_layer->coverage.data(), _layer->width, _layer->height};
        }
    }

    void multiThreadFunction(unsigned int threadID, unsigned int nThreads) override
    {
        const int h = _window.height();
        const int y0 = _window.y1 + int(std::size_t(threadID) * h / nThreads);
        const int y1 = _window.y1 + int(std::size_t(threadID + 1) * h / nThreads);

        // Hoisted out of the pixel loop: whether the elements move
        // independently is a property of the frame, not of the pixel.
        if (_layer)
        {
            for (int y = y0; y < y1; ++y)
            {
                float* row = _dst.row(y);
                for (int x = _window.x1; x < _window.x2; ++x)
                    RenderPixelLayered(_src, _mask, _layerColour, _layerCoverage, _cp, x, y,
                                       row + std::ptrdiff_t(x) * 4);
            }
            return;
        }

        for (int y = y0; y < y1; ++y)
        {
            float* row = _dst.row(y);
            for (int x = _window.x1; x < _window.x2; ++x)
                RenderPixel(_src, _mask, _cp, x, y, row + std::ptrdiff_t(x) * 4);
        }
    }

private:
    const ImageView& _src;
    const MutableImageView& _dst;
    const PlaneView& _mask;
    const CompositeParams& _cp;
    RectI _window;
    const ElementLayer* _layer;
    ImageView _layerColour;
    PlaneView _layerCoverage;
};

}  // namespace

// ============================================================== the instance

AutoMaskPlugin::AutoMaskPlugin(OfxImageEffectHandle p_Handle)
    : OFX::ImageEffect(p_Handle)
{
    _dstClip = fetchClip(kOfxImageEffectOutputClipName);
    _srcClip = fetchClip(kOfxImageEffectSimpleSourceClipName);

    _segMode        = fetchChoiceParam(kParamSegMode);
    _threshold      = fetchDoubleParam(kParamThreshold);
    _invertSource   = fetchBooleanParam(kParamInvertSource);
    _minElementSize = fetchIntParam(kParamMinElementSize);
    _bridgeGaps     = fetchIntParam(kParamBridgeGaps);
    _showAll        = fetchBooleanParam(kParamShowAll);
    _elementStatus  = fetchStringParam(kParamElementStatus);

    _selectedPoints = fetchStringParam(kParamSelectedPoints);
    _selectAll      = fetchPushButtonParam(kParamSelectAll);
    _clearSelection = fetchPushButtonParam(kParamClearSelection);

    _outputMode    = fetchChoiceParam(kParamOutputMode);
    _leaveOriginal = fetchBooleanParam(kParamLeaveOriginal);
    _invertMask    = fetchBooleanParam(kParamInvertMask);
    _expand        = fetchIntParam(kParamExpand);
    _feather       = fetchDoubleParam(kParamFeather);

    _xfPos       = fetchDouble2DParam(kParamXfPos);
    _xfScale     = fetchDoubleParam(kParamXfScale);
    _xfScaleY    = fetchDoubleParam(kParamXfScaleY);
    _xfLink      = fetchBooleanParam(kParamXfLink);
    _xfRot       = fetchDoubleParam(kParamXfRot);
    _xfAnchor    = fetchDouble2DParam(kParamXfAnchor);
    _anchorToSel = fetchPushButtonParam(kParamAnchorToSel);
    _autoCenter  = fetchPushButtonParam(kParamAutoCenter);

    _arrangeMode    = fetchChoiceParam(kParamArrangeMode);
    _arrangeOrder   = fetchChoiceParam(kParamArrangeOrder);
    _arrangeReverse = fetchBooleanParam(kParamArrangeReverse);
    _spread         = fetchDoubleParam(kParamSpread);
    _step           = fetchDouble2DParam(kParamStep);
    _alignH         = fetchChoiceParam(kParamAlignH);
    _alignV         = fetchChoiceParam(kParamAlignV);
    _filter      = fetchChoiceParam(kParamFilter);

    // The published documentation for Resolve's OFX host is thin and partly
    // wrong, so record what this one actually reports, once per process.
    am::ProbeHostOnce();
    syncVisibility();
}

am::DetectParams AutoMaskPlugin::DetectParamsAt(double time) const
{
    DetectParams d;
    d.mode = ChoiceAt(_segMode, time) == kSegLuminance ? SegmentMode::Luminance
                                                       : SegmentMode::Alpha;
    d.threshold = float(DoubleAt(_threshold, time));
    // Only meaningful in Luminance mode, and forced off elsewhere so a value
    // left over from a mode switch cannot act invisibly.
    d.invertSource = d.mode == SegmentMode::Luminance && BoolAt(_invertSource, time);
    d.minElementSize = IntAt(_minElementSize, time);
    d.bridgeRadius = IntAt(_bridgeGaps, time);
    return d;
}

AutoMaskPlugin::FrameParams AutoMaskPlugin::ReadFrameParams(double time) const
{
    FrameParams f;
    f.det = DetectParamsAt(time);

    f.mask.expand = IntAt(_expand, time);
    f.mask.feather = float(DoubleAt(_feather, time));
    f.mask.invert = BoolAt(_invertMask, time);

    f.outputMode = ChoiceAt(_outputMode, time);
    f.backgroundMode = BoolAt(_leaveOriginal, time) ? kBgLeft : kBgRemoved;
    f.filter = ChoiceAt(_filter, time) == kFilterOptNearest ? kFilterNearest : kFilterBilinear;

    Double2DAt(_xfPos, time, f.posX, f.posY);
    Double2DAt(_xfAnchor, time, f.anchorX, f.anchorY);
    f.scaleX = DoubleAt(_xfScale, time);
    // Linked means Scale Y is simply NOT READ. There is nothing to write back
    // and nothing to keep in step; the field is hidden precisely so the panel
    // cannot show a number that is not in force.
    f.scaleY = BoolAt(_xfLink, time) ? f.scaleX : DoubleAt(_xfScaleY, time);
    f.rotation = DoubleAt(_xfRot, time);

    f.arrange.mode = ChoiceAt(_arrangeMode, time);
    f.arrange.order = ChoiceAt(_arrangeOrder, time);
    f.arrange.reverse = BoolAt(_arrangeReverse, time);
    f.arrange.spread = DoubleAt(_spread, time);
    Double2DAt(_step, time, f.arrange.stepX, f.arrange.stepY);
    f.arrange.alignH = ChoiceAt(_alignH, time);
    f.arrange.alignV = ChoiceAt(_alignV, time);

    // Invert Mask and Arrange are mutually exclusive, and forcing it here
    // rather than trusting the UI keeps the two paths honest.
    //
    // An inverted mask is a statement about the whole frame -- everything
    // except the selection -- and that is not something individual elements can
    // each carry a piece of. There is no sensible answer to "move this element
    // and also invert", so the arrangement is dropped rather than producing one.
    if (f.mask.invert) f.arrange.mode = kArrangeNone;
    return f;
}

std::shared_ptr<const am::ElementLayer> AutoMaskPlugin::LayerFor(
    const ImageView& src, const std::shared_ptr<const Segmentation>& seg,
    const std::string& points, const FrameParams& fp) const
{
    std::lock_guard<std::mutex> lock(_layerCache.mutex);
    if (_layerCache.valid && _layerCache.seg == seg && _layerCache.points == points &&
        _layerCache.maskParams == fp.mask && _layerCache.arrangeParams == fp.arrange)
        return _layerCache.layer;

    std::shared_ptr<const ElementLayer> built;
    if (fp.arrange.mode != kArrangeNone && seg && !seg->empty())
    {
        const Resolved sel = ResolveSelection(*seg, ParseAnchors(points));
        const std::vector<Placement> placements = ComputeArrangement(
            *seg, sel, fp.arrange, float(seg->width), float(seg->height));

        // Settings that mean "no arrangement" -- Spread at 1.0, a zero step,
        // Align with both axes off -- must reach the single-pass path. Only
        // that path is exact at identity, because per-element feathering makes
        // the element coverages overlap slightly and their sum is no longer
        // exactly the combined mask.
        if (ArrangementMoves(placements))
        {
            auto layer = std::make_shared<ElementLayer>();
            BuildElementLayer(src, *seg, placements, fp.mask, fp.filter, *layer);
            built = layer;
        }
    }

    _layerCache.seg = seg;
    _layerCache.points = points;
    _layerCache.maskParams = fp.mask;
    _layerCache.arrangeParams = fp.arrange;
    _layerCache.layer = built;
    _layerCache.valid = true;
    return built;
}

am::CompositeParams AutoMaskPlugin::FrameParams::composite(float w, float h) const
{
    CompositeParams cp;
    cp.outputMode = outputMode;
    cp.backgroundMode = backgroundMode;
    cp.filter = filter;
    cp.identity = TransformIsIdentity(posX, posY, scaleX, scaleY, rotation);
    if (!cp.identity)
        cp.inv = mtx::Invert(
            MakeElementTransform(w, h, posX, posY, scaleX, scaleY, rotation, anchorX, anchorY));
    return cp;
}

bool AutoMaskPlugin::SelectionBounds(double /*time*/, const Segmentation& seg, RectI& out) const
{
    if (seg.empty()) return false;

    const Resolved sel = ResolveSelection(seg, ParseAnchors(StringOf(_selectedPoints)));
    out = RectI{};
    for (int l = 1; l <= seg.count(); ++l)
        if (sel.labelSelected[std::size_t(l)]) out.unionWith(seg.components[std::size_t(l)].bbox);
    return !out.empty();
}

std::shared_ptr<const am::Segmentation> AutoMaskPlugin::CurrentSegmentation() const
{
    std::lock_guard<std::mutex> lock(_cache.mutex);
    return _cache.seg;
}

std::string AutoMaskPlugin::SelectedPoints() const { return StringOf(_selectedPoints); }

void AutoMaskPlugin::SetSelectedPoints(const std::string& s)
{
    if (_selectedPoints) _selectedPoints->setValue(s);
}

bool AutoMaskPlugin::ShowAllElements() const { return BoolAt(_showAll, 0.0); }

// ------------------------------------------------------------------- render

void AutoMaskPlugin::render(const OFX::RenderArguments& p_Args)
{
    std::unique_ptr<OFX::Image> dst(_dstClip ? _dstClip->fetchImage(p_Args.time) : nullptr);
    if (!dst) return;

    std::unique_ptr<OFX::Image> src(
        _srcClip && _srcClip->isConnected() ? _srcClip->fetchImage(p_Args.time) : nullptr);

    const OfxRectI dstBounds = dst->getBounds();

    if (!src)
    {
        // Nothing connected. Emit transparent black rather than leaving the
        // host's buffer as it found it, which would show whatever was in it.
        for (int y = p_Args.renderWindow.y1; y < p_Args.renderWindow.y2; ++y)
        {
            float* row = static_cast<float*>(dst->getPixelAddress(p_Args.renderWindow.x1, y));
            if (row) std::fill(row, row + std::size_t(p_Args.renderWindow.x2 -
                                                      p_Args.renderWindow.x1) * 4, 0.0f);
        }
        return;
    }

#ifdef AUTOMASK_HAVE_CUDA
    if (p_Args.isEnabledCudaRender && CudaAvailable())
    {
        // No fallback on failure, deliberately. By this point the host has
        // handed over DEVICE pointers; the CPU loop below cannot touch them, so
        // "try the GPU, fall back to the CPU" is not a thing that exists here.
        if (!renderCuda(p_Args, src.get(), dst.get()))
            OFX::throwSuiteStatusException(kOfxStatFailed);
        return;
    }
#endif

    // Tiles and multi-resolution are off, so src and dst share bounds and one
    // top-down space serves both. The render window may still be a sub-rect of
    // them, which is why the analysis runs over the whole of `region` and only
    // the write is clipped: labelling a tile in isolation shatters every
    // element that crosses its edge.
    const OfxRectI region = dstBounds;
    const ImageView srcView = MakeSrcView(src.get(), region);
    const MutableImageView dstView = MakeDstView(dst.get(), region);
    if (!srcView.valid() || !dstView.valid()) return;

    const FrameParams fp = ReadFrameParams(p_Args.time);
    const DetectParams det = fp.det;

    // --- analysis, cached on content ----------------------------------------
    const std::uint64_t hash = HashSource(srcView, det);
    std::shared_ptr<const Segmentation> seg;
    {
        std::lock_guard<std::mutex> lock(_cache.mutex);
        if (hash != _cache.hash || srcView.width != _cache.width ||
            srcView.height != _cache.height || det != _cache.params || !_cache.seg)
        {
            _cache.hash = hash;
            _cache.width = srcView.width;
            _cache.height = srcView.height;
            _cache.params = det;
            _cache.seg = std::make_shared<const Segmentation>(Segment(srcView, det));
        }
        seg = _cache.seg;  // refcount bump, not a copy
    }

    // --- selection and mask --------------------------------------------------
    const std::string points = StringOf(_selectedPoints);
    const MaskParams mp = fp.mask;

    std::shared_ptr<const std::vector<float>> mask;
    {
        std::lock_guard<std::mutex> lock(_maskCache.mutex);
        if (_maskCache.seg != seg || _maskCache.points != points ||
            _maskCache.params != mp || !_maskCache.mask)
        {
            auto built = std::make_shared<std::vector<float>>();
            BuildMask(*seg, ResolveSelection(*seg, ParseAnchors(points)), mp, *built);

            _maskCache.seg = seg;
            _maskCache.points = points;
            _maskCache.params = mp;
            _maskCache.mask = built;
        }
        mask = _maskCache.mask;  // refcount bump, not a copy
    }

    // --- transform -----------------------------------------------------------
    const CompositeParams cp = fp.composite(float(srcView.width), float(srcView.height));

    // --- write ---------------------------------------------------------------
    // The render window arrives in host (bottom-up) coordinates; flip it into
    // the same top-down space as the views.
    RectI window{p_Args.renderWindow.x1 - region.x1, region.y2 - p_Args.renderWindow.y2,
                 p_Args.renderWindow.x2 - region.x1, region.y2 - p_Args.renderWindow.y1};
    window.x1 = std::max(window.x1, 0);
    window.y1 = std::max(window.y1, 0);
    window.x2 = std::min(window.x2, dstView.width);
    window.y2 = std::min(window.y2, dstView.height);
    if (window.empty()) return;

    const PlaneView plane{mask->data(), seg->width, seg->height};
    const std::shared_ptr<const ElementLayer> layer = LayerFor(srcView, seg, points, fp);

    BandRenderer renderer(srcView, dstView, plane, cp, window, layer.get());
    renderer.multiThread();

    // Deliberately NOT updating the status read-out here. Writing a parameter
    // from render is forbidden -- render runs on the host's worker threads,
    // outside any edit block, and a host that dispatches the resulting
    // changedParam back onto one of them deadlocks against the cache mutex this
    // function is already holding the result of. The read-out is refreshed from
    // changedParam instead, which is the only place a write is legal.
}

#ifdef AUTOMASK_HAVE_CUDA

AutoMaskPlugin::~AutoMaskPlugin()
{
    if (_cuda) CudaDestroy(_cuda);
}

bool AutoMaskPlugin::renderCuda(const OFX::RenderArguments& p_Args, OFX::Image* src,
                                OFX::Image* dst)
{
    std::lock_guard<std::mutex> lock(_cudaMutex);

    if (!_cuda)
    {
        _cuda = CudaCreate();
        if (!_cuda) return false;
    }

    const OfxRectI b = dst->getBounds();
    const int w = b.x2 - b.x1;
    const int h = b.y2 - b.y1;
    if (w <= 0 || h <= 0) return false;

    // Device pointers. These must NOT be dereferenced here -- only offset.
    const std::ptrdiff_t srcStride =
        std::ptrdiff_t(src->getRowBytes()) / std::ptrdiff_t(sizeof(float));
    const std::ptrdiff_t dstStride =
        std::ptrdiff_t(dst->getRowBytes()) / std::ptrdiff_t(sizeof(float));
    auto* srcBase = static_cast<const float*>(src->getPixelData());
    auto* dstBase = static_cast<float*>(dst->getPixelData());
    if (!srcBase || !dstBase || srcStride <= 0 || dstStride <= 0) return false;

    void* stream = p_Args.pCudaStream;
    const FrameParams fp = ReadFrameParams(p_Args.time);

    // --- analysis, keyed on a fingerprint computed ON THE DEVICE -------------
    //
    // This is what keeps the labelling on the CPU from costing a full frame
    // download every frame: the hash comes back as eight bytes, and the 33 MB
    // only moves when the content has actually changed.
    const std::uint64_t hash =
        CudaHashSource(_cuda, srcBase, w, h, srcStride,
                       fp.det.mode == SegmentMode::Luminance ? 1 : 0, fp.det.threshold, stream);

    std::shared_ptr<const Segmentation> seg;
    {
        std::lock_guard<std::mutex> clock(_cache.mutex);
        if (hash != _cache.hash || w != _cache.width || h != _cache.height ||
            fp.det != _cache.params || !_cache.seg)
        {
            _downloadBuf.resize(std::size_t(w) * h * 4);
            if (!CudaDownload(_cuda, srcBase, w, h, srcStride, _downloadBuf.data(), stream))
                return false;

            // The download is bottom-up, as the host image is. A top-down view
            // of it is a negative row stride from the last row -- no flip pass,
            // exactly as on the CPU path.
            const ImageView topDown{_downloadBuf.data() + std::size_t(h - 1) * w * 4, w, h,
                                    -std::ptrdiff_t(w) * 4};

            _cache.hash = hash;
            _cache.width = w;
            _cache.height = h;
            _cache.params = fp.det;
            _cache.seg = std::make_shared<const Segmentation>(Segment(topDown, fp.det));
        }
        seg = _cache.seg;
    }

    const std::string points = StringOf(_selectedPoints);
    const Resolved sel = ResolveSelection(*seg, ParseAnchors(points));

    if (_cudaLabelsFor != seg)
    {
        if (!CudaUploadLabels(_cuda, seg->labelImage.data(), w, h, stream)) return false;
        _cudaLabelsFor = seg;
        _cudaMaskValid = false;
    }

    // One byte per element, so there is no point caching this separately.
    if (!CudaUploadSelection(_cuda, sel.labelSelected.data(), seg->count(), stream))
        return false;

    if (!_cudaMaskValid || _cudaMaskSeg != seg || _cudaMaskPoints != points ||
        _cudaMaskParams != fp.mask)
    {
        if (!CudaBuildMask(_cuda, w, h, fp.mask.expand, fp.mask.feather, fp.mask.invert, stream))
            return false;
        _cudaMaskSeg = seg;
        _cudaMaskPoints = points;
        _cudaMaskParams = fp.mask;
        _cudaMaskValid = true;
    }

    // --- the arranged element layer -----------------------------------------
    //
    // Built on the host, which needs host pixels. _downloadBuf already holds
    // them for the current content -- it is only refilled when the hash says
    // the content changed -- but a CPU render earlier in the session could have
    // filled the analysis cache without ever filling it, so check rather than
    // assume.
    std::shared_ptr<const ElementLayer> layer;
    if (fp.arrange.mode != kArrangeNone)
    {
        if (_downloadBuf.size() != std::size_t(w) * h * 4)
        {
            _downloadBuf.resize(std::size_t(w) * h * 4);
            if (!CudaDownload(_cuda, srcBase, w, h, srcStride, _downloadBuf.data(), stream))
                return false;
        }
        const ImageView topDown{_downloadBuf.data() + std::size_t(h - 1) * w * 4, w, h,
                                -std::ptrdiff_t(w) * 4};
        layer = LayerFor(topDown, seg, points, fp);
    }

    if (layer)
    {
        // Uploaded only when the layer object itself changes. It is built in
        // arranged space, so Position, Scale and Rotation do not touch it.
        if (_cudaLayer != layer)
        {
            if (!CudaUploadLayer(_cuda, layer->colour.data(), layer->coverage.data(), w, h,
                                 stream))
                return false;
            _cudaLayer = layer;
        }
    }
    else if (_cudaLayer)
    {
        CudaClearLayer(_cuda);
        _cudaLayer.reset();
    }

    // Top-down views of both images, as negative strides from the last row, so
    // the kernel indexes the same space the label image and mask are in.
    const CompositeParams cp = fp.composite(float(w), float(h));
    const float* srcTop = srcBase + srcStride * (h - 1);
    float* dstTop = dstBase + dstStride * (h - 1);

    if (layer)
        return CudaRenderLayered(_cuda, srcTop, dstTop, w, h, -srcStride, -dstStride, cp,
                                 stream);
    return CudaRender(_cuda, srcTop, dstTop, w, h, -srcStride, -dstStride, cp, stream);
}

#else

AutoMaskPlugin::~AutoMaskPlugin() = default;

#endif  // AUTOMASK_HAVE_CUDA

// -------------------------------------------------------------- changedParam

void AutoMaskPlugin::changedParam(const OFX::InstanceChangedArgs& p_Args,
                                  const std::string& p_ParamName)
{
    try
    {
        if (p_ParamName == kParamXfLink || p_ParamName == kParamSegMode ||
            p_ParamName == kParamOutputMode || p_ParamName == kParamArrangeMode ||
            p_ParamName == kParamInvertMask)
        {
            syncVisibility();
            return;
        }

        if (p_ParamName == kParamClearSelection)
        {
            EditBlock eb(this, "Clear Selection");
            SetSelectedPoints("");
            updateStatus();
            return;
        }

        if (p_ParamName == kParamSelectAll)
        {
            std::shared_ptr<const Segmentation> seg = CurrentSegmentation();
            if (!seg || seg->empty())
            {
                // Nothing has been analysed yet, so there is nothing to select.
                // Say so rather than silently doing nothing.
                if (_elementStatus)
                    _elementStatus->setValue("No analysis yet - step a frame first.");
                return;
            }
            EditBlock eb(this, "Select All Elements");
            SetSelectedPoints(FormatAnchors(AnchorsForAll(*seg)));
            updateStatus();
            return;
        }

        if (p_ParamName == kParamAnchorToSel || p_ParamName == kParamAutoCenter)
        {
            std::shared_ptr<const Segmentation> seg = CurrentSegmentation();
            RectI box;
            if (!seg || !SelectionBounds(p_Args.time, *seg, box))
            {
                if (_elementStatus)
                    _elementStatus->setValue(
                        seg && !seg->empty() ? "Nothing selected." : "No analysis yet.");
                return;
            }

            const double cxPx = (box.x1 + box.x2) * 0.5;
            const double cyPx = (box.y1 + box.y2) * 0.5;

            if (p_ParamName == kParamAnchorToSel)
            {
                // Back to normalized and back to the host's y-up convention. The
                // default anchor is the centre of frame, which is the wrong pivot
                // for rotating one element.
                EditBlock eb(this, "Anchor to Selection");
                if (_xfAnchor)
                    _xfAnchor->setValue(cxPx / double(seg->width),
                                        1.0 - cyPx / double(seg->height));
                return;
            }

            // Auto Center. Writes Position and nothing else, so the framing is
            // still yours to nudge afterwards.
            double ax = 0.5, ay = 0.5;
            Double2DAt(_xfAnchor, p_Args.time, ax, ay);
            const double sx = DoubleAt(_xfScale, p_Args.time);
            const double sy = BoolAt(_xfLink, p_Args.time) ? sx
                                                           : DoubleAt(_xfScaleY, p_Args.time);
            const double rot = DoubleAt(_xfRot, p_Args.time);

            const Resolved sel =
                ResolveSelection(*seg, ParseAnchors(StringOf(_selectedPoints)));

            double posX = 0.0, posY = 0.0;
            if (!CenterOffsetForSelection(*seg, sel, sx, sy, rot, ax, ay, posX, posY)) return;

            EditBlock eb(this, "Auto Center");
            if (_xfPos) _xfPos->setValue(posX, posY);
            return;
        }

        if (p_ParamName == kParamSelectedPoints || p_ParamName == kParamThreshold ||
            p_ParamName == kParamMinElementSize || p_ParamName == kParamBridgeGaps)
        {
            updateStatus();
            return;
        }
    }
    catch (...)
    {
        // A throw out of changedParam takes the host's parameter dispatch with
        // it. Whatever went wrong, a stale panel beats a dead one.
    }
    (void)p_Args;
}

void AutoMaskPlugin::syncVisibility()
{
    // One place, one pass.
    //
    // Every visibility rule lives here and nowhere else. Split across several
    // independent passes they overwrite each other and strand a control hidden
    // -- a bug the user cannot even report, because the evidence is invisible.
    try
    {
        if (_xfScaleY && _xfLink)
        {
            bool link = true;
            _xfLink->getValue(link);
            _xfScaleY->setIsSecret(link);
        }
        if (_invertSource && _segMode)
        {
            int mode = kSegAlpha;
            _segMode->getValue(mode);
            // In Alpha mode, below-threshold alpha IS background by definition;
            // inverting it yields a hollow frame that only ever looks like a bug.
            _invertSource->setIsSecret(mode != kSegLuminance);
        }
        if (_leaveOriginal && _outputMode)
        {
            int out = kOutMatte;
            _outputMode->getValue(out);
            _leaveOriginal->setIsSecret(out != kOutComposite);
        }

        {
            int mode = kArrangeNone;
            if (_arrangeMode) _arrangeMode->getValue(mode);
            bool inverted = false;
            if (_invertMask) _invertMask->getValue(inverted);

            // Arrange cannot mean anything against an inverted mask, so the
            // whole section goes rather than sitting there doing nothing. The
            // read side forces it off too -- this only stops the panel showing
            // controls that are not in force.
            const bool off = inverted || mode == kArrangeNone;
            if (_arrangeOrder) _arrangeOrder->setIsSecret(off);
            if (_arrangeReverse) _arrangeReverse->setIsSecret(off);
            if (_spread) _spread->setIsSecret(inverted || mode != kArrangeSpread);
            if (_step) _step->setIsSecret(inverted || mode != kArrangeStagger);
            if (_alignH) _alignH->setIsSecret(inverted || mode != kArrangeAlign);
            if (_alignV) _alignV->setIsSecret(inverted || mode != kArrangeAlign);
            if (_arrangeMode) _arrangeMode->setEnabled(!inverted);
        }
    }
    catch (...)
    {
    }
}

void AutoMaskPlugin::updateStatus()
{
    if (!_elementStatus) return;
    try
    {
        std::shared_ptr<const Segmentation> seg = CurrentSegmentation();
        if (!seg || seg->width == 0)
        {
            _elementStatus->setValue("No analysis yet - step a frame first.");
            return;
        }
        const Resolved sel = ResolveSelection(*seg, ParseAnchors(StringOf(_selectedPoints)));

        char buf[160];
        if (sel.unresolvedCount > 0)
            std::snprintf(buf, sizeof(buf), "%d found, %d selected, %d point(s) hit nothing",
                          seg->count(), sel.selectedCount, sel.unresolvedCount);
        else
            std::snprintf(buf, sizeof(buf), "%d found, %d selected", seg->count(),
                          sel.selectedCount);
        _elementStatus->setValue(buf);
    }
    catch (...)
    {
    }
}

// ================================================================== factory

namespace {

int g_paramCount = 0;

template <class T> int KindOf(T*)                { return kPKNone; }
inline int KindOf(OFX::DoubleParamDescriptor*)   { return kPKDouble; }
inline int KindOf(OFX::Double2DParamDescriptor*) { return kPKDouble2D; }
inline int KindOf(OFX::IntParamDescriptor*)      { return kPKInt; }
inline int KindOf(OFX::BooleanParamDescriptor*)  { return kPKBool; }
inline int KindOf(OFX::ChoiceParamDescriptor*)   { return kPKChoice; }
inline int KindOf(OFX::StringParamDescriptor*)   { return kPKString; }

/// Every value-bearing parameter goes through here.
///
/// The OFX default, eCacheInvalidateValueChange, means "invalidate only the
/// frames this parameter's keyframe covers". That is right for a purely
/// keyframed effect and wrong here: a change to Threshold re-segments, which
/// can change every frame. Resolve tolerates the default; Fusion does not, and
/// Fusion is the one behaving correctly.
template <class T> T* AddParam(OFX::PageParamDescriptor* page, T* p)
{
    p->setCacheInvalidation(OFX::eCacheInvalidateValueAll);
    page->addChild(*p);
    ++g_paramCount;
    RegisterParam(p->getName(), KindOf(p));
    return p;
}

/// For controls that change nothing about the rendered image -- the overlay
/// toggle, the status line. Purging every cached frame to show a widget is a
/// real stall for no pixels.
template <class T> T* AddParamUI(OFX::PageParamDescriptor* page, T* p)
{
    page->addChild(*p);
    ++g_paramCount;
    return p;
}

OFX::GroupParamDescriptor* DefineGroup(OFX::ImageEffectDescriptor& desc,
                                       OFX::PageParamDescriptor* page, const char* name,
                                       const char* label)
{
    OFX::GroupParamDescriptor* g = desc.defineGroupParam(name);
    g->setLabels(label, label, label);
    g->setOpen(false);
    page->addChild(*g);
    return g;
}

OFX::DoubleParamDescriptor* DefineDouble(OFX::ImageEffectDescriptor& desc,
                                         OFX::PageParamDescriptor* page,
                                         OFX::GroupParamDescriptor* parent, const char* name,
                                         const char* label, const char* hint, double def,
                                         double lo, double hi, double dLo, double dHi,
                                         double inc = 0.001)
{
    OFX::DoubleParamDescriptor* p = desc.defineDoubleParam(name);
    p->setLabels(label, label, label);
    p->setHint(hint);
    p->setDefault(def);
    p->setRange(lo, hi);
    p->setDisplayRange(dLo, dHi);
    p->setIncrement(inc);
    p->setDoubleType(OFX::eDoubleTypePlain);
    if (parent) p->setParent(*parent);
    return AddParam(page, p);
}

OFX::Double2DParamDescriptor* DefineXY(OFX::ImageEffectDescriptor& desc,
                                       OFX::PageParamDescriptor* page,
                                       OFX::GroupParamDescriptor* parent, const char* name,
                                       const char* label, const char* hint, double defX,
                                       double defY, double dLo, double dHi)
{
    OFX::Double2DParamDescriptor* p = desc.defineDouble2DParam(name);
    p->setLabels(label, label, label);
    p->setHint(hint);
    p->setDefault(defX, defY);
    p->setRange(-1000.0, -1000.0, 1000.0, 1000.0);
    p->setDisplayRange(dLo, dLo, dHi, dHi);
    p->setIncrement(0.001);
    p->setDoubleType(OFX::eDoubleTypePlain);
    if (parent) p->setParent(*parent);
    return AddParam(page, p);
}

OFX::IntParamDescriptor* DefineInt(OFX::ImageEffectDescriptor& desc,
                                   OFX::PageParamDescriptor* page,
                                   OFX::GroupParamDescriptor* parent, const char* name,
                                   const char* label, const char* hint, int def, int lo, int hi,
                                   int dLo, int dHi)
{
    OFX::IntParamDescriptor* p = desc.defineIntParam(name);
    p->setLabels(label, label, label);
    p->setHint(hint);
    p->setDefault(def);
    p->setRange(lo, hi);
    p->setDisplayRange(dLo, dHi);
    if (parent) p->setParent(*parent);
    return AddParam(page, p);
}

OFX::BooleanParamDescriptor* DefineBool(OFX::ImageEffectDescriptor& desc,
                                        OFX::PageParamDescriptor* page,
                                        OFX::GroupParamDescriptor* parent, const char* name,
                                        const char* label, const char* hint, bool def)
{
    OFX::BooleanParamDescriptor* p = desc.defineBooleanParam(name);
    p->setLabels(label, label, label);
    p->setHint(hint);
    p->setDefault(def);
    if (parent) p->setParent(*parent);
    return AddParam(page, p);
}

OFX::PushButtonParamDescriptor* DefineButton(OFX::ImageEffectDescriptor& desc,
                                             OFX::PageParamDescriptor* page,
                                             OFX::GroupParamDescriptor* parent, const char* name,
                                             const char* label, const char* hint)
{
    OFX::PushButtonParamDescriptor* p = desc.definePushButtonParam(name);
    p->setLabels(label, label, label);
    p->setHint(hint);
    if (parent) p->setParent(*parent);
    return AddParamUI(page, p);
}

/// A line of text the plugin writes and the user only reads.
OFX::StringParamDescriptor* DefineReadout(OFX::ImageEffectDescriptor& desc,
                                          OFX::PageParamDescriptor* page,
                                          OFX::GroupParamDescriptor* parent, const char* name,
                                          const char* label, const char* def)
{
    OFX::StringParamDescriptor* p = desc.defineStringParam(name);
    p->setLabels(label, label, label);
    p->setStringType(OFX::eStringTypeSingleLine);
    p->setDefault(def);
    p->setEnabled(false);
    p->setAnimates(false);
    p->setIsPersistant(false);
    if (parent) p->setParent(*parent);
    return AddParamUI(page, p);
}

}  // namespace

class AutoMaskPluginFactory : public OFX::PluginFactoryHelper<AutoMaskPluginFactory>
{
public:
    AutoMaskPluginFactory()
        : OFX::PluginFactoryHelper<AutoMaskPluginFactory>(kPluginIdentifier, kPluginVersionMajor,
                                                          kPluginVersionMinor)
    {
    }

    void describe(OFX::ImageEffectDescriptor& p_Desc) override;
    void describeInContext(OFX::ImageEffectDescriptor& p_Desc,
                           OFX::ContextEnum p_Context) override;
    OFX::ImageEffect* createInstance(OfxImageEffectHandle p_Handle,
                                     OFX::ContextEnum p_Context) override;
};

void AutoMaskPluginFactory::describe(OFX::ImageEffectDescriptor& p_Desc)
{
    p_Desc.setLabels(kPluginName, kPluginName, kPluginName);
    p_Desc.setPluginGrouping(kPluginGrouping);
    p_Desc.setPluginDescription(kPluginDescription);

    p_Desc.addSupportedContext(OFX::eContextFilter);
    p_Desc.addSupportedContext(OFX::eContextGeneral);
    p_Desc.addSupportedBitDepth(OFX::eBitDepthFloat);

    p_Desc.setSingleInstance(false);
    p_Desc.setHostFrameThreading(false);
    p_Desc.setRenderThreadSafety(OFX::eRenderInstanceSafe);
    p_Desc.setTemporalClipAccess(false);
    p_Desc.setRenderTwiceAlways(false);
    p_Desc.setSupportsMultipleClipPARs(false);

    // Not cosmetic. Connected-component labelling over a tile labels each tile
    // independently, so every element crossing a tile boundary shatters into
    // pieces that then get separate labels and separate selection state.
    p_Desc.setSupportsTiles(false);
    p_Desc.setSupportsMultiResolution(false);

    // In describe(), NOT describeInContext(): the overlay descriptor is a
    // plugin-level property and the host caches the descriptor, so it is not
    // reliably picked up from the per-context pass.
    am::RegisterOverlay(p_Desc);

#ifdef AUTOMASK_HAVE_CUDA
    // Declared only when a device actually exists. Claiming CUDA on a machine
    // with no GPU would have the host hand over device pointers that cannot be
    // serviced, and by then there is no way back to the CPU loop -- the pixel
    // addresses are already device memory.
    if (am::CudaAvailable())
    {
        p_Desc.setSupportsCudaRender(true);
        // Without this Resolve pulls every frame down to system memory and
        // pushes the result back, which is most of what the GPU path exists to
        // avoid.
        p_Desc.setSupportsCudaStream(true);
    }
#endif
}

void AutoMaskPluginFactory::describeInContext(OFX::ImageEffectDescriptor& p_Desc,
                                              OFX::ContextEnum /*p_Context*/)
{
    OFX::ClipDescriptor* srcClip = p_Desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    srcClip->addSupportedComponent(OFX::ePixelComponentRGBA);
    srcClip->setTemporalClipAccess(false);
    srcClip->setSupportsTiles(false);
    srcClip->setIsMask(false);

    OFX::ClipDescriptor* dstClip = p_Desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(OFX::ePixelComponentRGBA);
    dstClip->setSupportsTiles(false);

    // Resolve reports maxPages = 0, so there is no point splitting the controls
    // across pages. One page, groups for structure.
    OFX::PageParamDescriptor* page = p_Desc.definePageParam("Controls");

    // --- Segmentation --------------------------------------------------------
    {
        OFX::GroupParamDescriptor* g = DefineGroup(p_Desc, page, "grpSegment", "Segmentation");

        OFX::ChoiceParamDescriptor* mode = p_Desc.defineChoiceParam(kParamSegMode);
        mode->setLabels("Segment By", "Segment By", "Segment By");
        mode->setHint(
            "What separates one element from another. Alpha splits the frame wherever it is "
            "transparent, which is what you want for a PNG with a real alpha channel or the "
            "output of a title generator. Luminance splits on brightness instead, for opaque "
            "artwork sitting on a flat background.");
        mode->appendOption("Alpha");
        mode->appendOption("Luminance");
        mode->setDefault(kSegAlpha);
        mode->setAnimates(false);
        mode->setParent(*g);
        AddParam(page, mode);

        DefineDouble(p_Desc, page, g, kParamThreshold, "Threshold",
                     "Where the cut between element and background falls. Raise it to shed "
                     "soft antialiased fringes; lower it to keep faint edges attached.",
                     0.15, 0.0, 1.0, 0.0, 1.0);

        DefineBool(p_Desc, page, g, kParamInvertSource, "Invert Source",
                   "Treat pixels BELOW the threshold as the elements. For dark artwork on a "
                   "light background. Luminance mode only -- in Alpha mode, below the "
                   "threshold is background by definition.",
                   false);

        DefineInt(p_Desc, page, g, kParamMinElementSize, "Min Element Size",
                  "Elements smaller than this many pixels are discarded. Antialiasing debris "
                  "and stray dots are not elements, and dropping them keeps the count -- and "
                  "Select All -- meaningful.",
                  16, 0, 1000000, 0, 500);

        DefineInt(p_Desc, page, g, kParamBridgeGaps, "Bridge Gaps",
                  "Grows the artwork by this many pixels before splitting it, so pieces that "
                  "nearly touch are treated as one element. Use it when a dotted outline or a "
                  "thin join shatters into fragments. Bounding boxes are unaffected: the "
                  "growth only decides what joins to what.",
                  0, 0, 64, 0, 16);

        OFX::BooleanParamDescriptor* showAll = p_Desc.defineBooleanParam(kParamShowAll);
        showAll->setLabels("Show All Elements", "Show All Elements", "Show All Elements");
        showAll->setHint(
            "Outlines every element found, not just the selected ones, so you can see what "
            "the threshold is actually doing. Viewer only -- it changes nothing in the render, "
            "so it can be left on without costing a re-render.");
        showAll->setDefault(false);
        showAll->setAnimates(false);
        showAll->setParent(*g);
        AddParamUI(page, showAll);

        // Refreshed from changedParam, which runs before the render that
        // produces the new analysis -- so after a threshold change this shows
        // the previous count until the next edit. The viewer overlay draws the
        // live figure; this exists for when the overlay is switched off.
        DefineReadout(p_Desc, page, g, kParamElementStatus, "Elements (last)",
                      "No analysis yet - step a frame first.");
    }

    // --- Selection -----------------------------------------------------------
    {
        OFX::GroupParamDescriptor* g = DefineGroup(p_Desc, page, "grpSelection", "Selection");

        OFX::StringParamDescriptor* pts = p_Desc.defineStringParam(kParamSelectedPoints);
        pts->setLabels("Selected Points", "Selected Points", "Selected Points");
        pts->setHint(
            "One normalized point per selected element, written by clicking in the viewer. "
            "Left visible on purpose: a selection can be copied to another clip by pasting "
            "this string, and a point that has stopped matching anything can be deleted by "
            "hand.");
        pts->setStringType(OFX::eStringTypeSingleLine);
        pts->setDefault("");
        // Points name places, not times. Animating them would mean the selection
        // silently differed frame to frame with nothing on screen to say so.
        pts->setAnimates(false);
        pts->setParent(*g);
        AddParam(page, pts);

        DefineButton(p_Desc, page, g, kParamSelectAll, "Select All",
                     "Selects every element the current settings found.");
        DefineButton(p_Desc, page, g, kParamClearSelection, "Clear Selection",
                     "Drops every selected element.");
    }

    // --- Mask ----------------------------------------------------------------
    {
        OFX::GroupParamDescriptor* g = DefineGroup(p_Desc, page, "grpMask", "Mask");

        OFX::ChoiceParamDescriptor* out = p_Desc.defineChoiceParam(kParamOutputMode);
        out->setLabels("Output", "Output", "Output");
        out->setHint(
            "Preview shows the selection at full strength over a dimmed copy of the rest, so "
            "you can see what you are clicking. Switch to one of the others when you are "
            "done. Matte gives a black-and-white mask to drive something downstream. Cutout "
            "keeps the original colour and knocks out everything unselected. Cutout "
            "(Premultiplied) is the same already multiplied by the mask, for compositing "
            "straight on top of another layer. Composite keeps the whole frame and moves the "
            "selection within it, which is the one to use with the Transform below.");
        // Appended, never inserted: the host stores the index, so adding an
        // option in the middle would reinterpret every project already saved.
        out->appendOption("Matte");
        out->appendOption("Cutout");
        out->appendOption("Cutout (Premultiplied)");
        out->appendOption("Composite");
        out->appendOption("Preview");
        // Preview is the default because every other mode hides what is not
        // selected -- so on a fresh instance, with nothing selected yet, they
        // all show a blank frame and there is nothing to click.
        out->setDefault(kOutPreview);
        out->setAnimates(false);
        out->setParent(*g);
        AddParam(page, out);

        DefineBool(p_Desc, page, g, kParamLeaveOriginal, "Leave Original In Place",
                   "Composite only. Normally the element is lifted out and its old position "
                   "becomes transparent, so moving it moves it. Turn this on to leave a copy "
                   "behind instead -- the one-click way to duplicate an element.",
                   false);

        DefineBool(p_Desc, page, g, kParamInvertMask, "Invert Mask",
                   "Swaps what is kept for what is thrown away. Applied last, so Expand still "
                   "grows the elements you picked rather than the hole around them.",
                   false);

        DefineInt(p_Desc, page, g, kParamExpand, "Expand / Contract",
                  "Grows the mask outward, or inward when negative. Useful for pulling the "
                  "edge in past a fringe, or pushing it out to cover one. The growth is "
                  "square rather than round, so a sharp corner gains slightly more than a "
                  "flat edge.",
                  0, -256, 256, -32, 32);

        DefineDouble(p_Desc, page, g, kParamFeather, "Feather",
                     "Width of the soft edge, in pixels. Zero leaves the mask hard and skips "
                     "the blur entirely.",
                     0.0, 0.0, 1000.0, 0.0, 64.0, 0.1);
    }

    // --- Transform -----------------------------------------------------------
    {
        OFX::GroupParamDescriptor* g = DefineGroup(p_Desc, page, "grpTransform", "Transform");

        DefineXY(p_Desc, page, g, kParamXfPos, "Position",
                 "Moves the selected elements, as a fraction of the frame. Set Output to "
                 "Composite to see them move within the picture rather than on their own.",
                 0.0, 0.0, -1.0, 1.0);

        // Two separate scalars rather than one Double2D with the axes mirrored.
        // Mirroring means writing the other axis back through the host on every
        // change: a round trip you can watch happen, and one that loses track if
        // the host coalesces events, leaving the two stuck out of step. There is
        // nothing to write here. When linked, Scale Y is simply not read, and it
        // is hidden so the panel cannot show a number that is not in force.
        DefineDouble(p_Desc, page, g, kParamXfScale, "Scale", "About the Anchor.", 1.0, -1000.0,
                     1000.0, 0.0, 3.0);
        DefineDouble(p_Desc, page, g, kParamXfScaleY, "Scale Y",
                     "Only used when Link Scale is off.", 1.0, -1000.0, 1000.0, 0.0, 3.0);
        DefineBool(p_Desc, page, g, kParamXfLink, "Link Scale",
                   "Scales proportionally from the one Scale value. Turn off to stretch one "
                   "axis, which reveals Scale Y.",
                   true);

        DefineDouble(p_Desc, page, g, kParamXfRot, "Rotation", "About the Anchor, in degrees.",
                     0.0, -100000.0, 100000.0, -180.0, 180.0, 1.0);

        DefineXY(p_Desc, page, g, kParamXfAnchor, "Anchor",
                 "The point scaling and rotation happen about, as a fraction of the frame. The "
                 "default is the centre of frame, which is rarely the right pivot for a single "
                 "element -- use the button below.",
                 0.5, 0.5, 0.0, 1.0);

        DefineButton(p_Desc, page, g, kParamAnchorToSel, "Anchor to Selection",
                     "Moves the Anchor to the centre of whatever is currently selected.");

        DefineButton(p_Desc, page, g, kParamAutoCenter, "Auto Center",
                     "Sets Position so the selected elements sit in the middle of the frame. "
                     "Takes the current Scale and Rotation into account, so it centres the "
                     "element as it actually appears rather than as it started. Writes "
                     "Position only -- everything else is left where you put it.");

        OFX::ChoiceParamDescriptor* filt = p_Desc.defineChoiceParam(kParamFilter);
        filt->setLabels("Filter", "Filter", "Filter");
        filt->setHint(
            "How pixels are sampled when the transform lands between them. Bilinear is smooth; "
            "Nearest keeps hard pixel edges, for pixel art.");
        filt->appendOption("Nearest");
        filt->appendOption("Bilinear");
        filt->setDefault(kFilterOptBilinear);
        filt->setAnimates(false);
        filt->setParent(*g);
        AddParam(page, filt);
    }

    // --- Arrange -------------------------------------------------------------
    {
        OFX::GroupParamDescriptor* g = DefineGroup(p_Desc, page, "grpArrange", "Arrange");

        OFX::ChoiceParamDescriptor* mode = p_Desc.defineChoiceParam(kParamArrangeMode);
        mode->setLabels("Mode", "Mode", "Mode");
        mode->setHint(
            "Moves the selected elements relative to EACH OTHER, before the Transform above "
            "moves them as a group. Spread pushes them apart from their shared centre. "
            "Stagger steps each one along by a fixed amount, in the chosen Order, for a "
            "cascade. Align snaps them to a common edge or centre. Unavailable while Invert "
            "Mask is on, since there are no individual elements to move in an inverted mask.");
        mode->appendOption("None");
        mode->appendOption("Spread");
        mode->appendOption("Stagger");
        mode->appendOption("Align");
        mode->setDefault(kArrangeNone);
        mode->setParent(*g);
        AddParam(page, mode);

        OFX::ChoiceParamDescriptor* order = p_Desc.defineChoiceParam(kParamArrangeOrder);
        order->setLabels("Order", "Order", "Order");
        order->setHint(
            "Which element counts as first. Drives the Stagger step, and decides which "
            "element sits on top where two of them overlap.");
        order->appendOption("Left to Right");
        order->appendOption("Right to Left");
        order->appendOption("Top to Bottom");
        order->appendOption("Bottom to Top");
        order->appendOption("Reading Order");
        order->appendOption("Largest First");
        order->appendOption("Smallest First");
        order->appendOption("Nearest Centre First");
        order->setDefault(kOrderLeftRight);
        order->setParent(*g);
        AddParam(page, order);

        DefineBool(p_Desc, page, g, kParamArrangeReverse, "Reverse Order",
                   "Flips the order end for end, so a cascade runs the other way.", false);

        DefineDouble(p_Desc, page, g, kParamSpread, "Spread",
                     "How far apart to push the elements, as a multiple of their existing "
                     "distance from the shared centre. 1.0 leaves them exactly where they "
                     "are; 2.0 doubles every gap. Being a multiplier rather than a distance, "
                     "one value works whatever the spacing was.",
                     1.0, -100.0, 100.0, 0.0, 4.0, 0.01);

        DefineXY(p_Desc, page, g, kParamStep, "Step",
                 "How far each element moves past the one before it, as a fraction of the "
                 "frame. The first element in the Order does not move at all.",
                 0.0, 0.0, -0.5, 0.5);

        OFX::ChoiceParamDescriptor* ah = p_Desc.defineChoiceParam(kParamAlignH);
        ah->setLabels("Align Horizontal", "Align Horizontal", "Align Horizontal");
        ah->setHint("Snaps every element to the same left edge, centre, or right edge.");
        ah->appendOption("None");
        ah->appendOption("Left");
        ah->appendOption("Center");
        ah->appendOption("Right");
        ah->setDefault(kAlignHNone);
        ah->setParent(*g);
        AddParam(page, ah);

        OFX::ChoiceParamDescriptor* av = p_Desc.defineChoiceParam(kParamAlignV);
        av->setLabels("Align Vertical", "Align Vertical", "Align Vertical");
        av->setHint("Snaps every element to the same top, middle, or bottom.");
        av->appendOption("None");
        av->appendOption("Top");
        av->appendOption("Middle");
        av->appendOption("Bottom");
        av->setDefault(kAlignVNone);
        av->setParent(*g);
        AddParam(page, av);
    }

    // --- About ---------------------------------------------------------------
    {
        OFX::GroupParamDescriptor* g = DefineGroup(p_Desc, page, "grpAbout", "About");
        DefineReadout(p_Desc, page, g, kParamOverlayHint, "Clicking",
                      "Set the viewer's on-screen-control dropdown to 'Open FX Overlay'.");
    }
}

OFX::ImageEffect* AutoMaskPluginFactory::createInstance(OfxImageEffectHandle p_Handle,
                                                        OFX::ContextEnum /*p_Context*/)
{
    return new AutoMaskPlugin(p_Handle);
}

void OFX::Plugin::getPluginIDs(OFX::PluginFactoryArray& p_FactoryArray)
{
    static AutoMaskPluginFactory factory;
    p_FactoryArray.push_back(&factory);
}
