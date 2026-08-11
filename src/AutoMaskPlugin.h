#pragma once

#include "ofxsImageEffect.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/MaskBuild.h"
#include "core/Segmentation.h"

#ifdef AUTOMASK_HAVE_CUDA
#include "render/CudaRender.h"
#endif

#define kPluginName        "Auto Mask"
#define kPluginGrouping    "AutoMask"
#define kPluginDescription                                                              \
    "Isolates the discrete elements in a frame by pixel value, and masks the ones you " \
    "pick. Click an element in the viewer to select it, click again to drop it. The "   \
    "transform then moves just what you selected."
#define kPluginIdentifier  "com.resolveautomask.AutoMask"
#define kPluginVersionMajor 0
#define kPluginVersionMinor 1

namespace am {

/// The result of one analysis, plus everything needed to decide whether it is
/// still valid.
///
/// Modelled on resolve-text-animator's AnalysisCache, minus the per-grouping-
/// mode variants it needs and this does not: here DetectParams compares whole.
struct AnalysisCache {
    std::mutex mutex;
    std::uint64_t hash = 0;
    int width = 0;
    int height = 0;
    DetectParams params;

    /// shared_ptr<const> is load-bearing, not decoration. A Segmentation holds
    /// one int32 per pixel: 8 MB at 1080p, 33 MB at UHD. Returning it by value
    /// would memcpy that every frame while holding the mutex. A cache hit has
    /// to be a refcount bump.
    std::shared_ptr<const Segmentation> seg;
};

/// The built mask, cached on everything it depends on.
///
/// Worth its own cache because the dependencies are strictly narrower than the
/// frame: the mask changes only when the segmentation, the selection or the
/// mask controls change. Scrubbing a still graphic, or playing one across a
/// hundred frames, re-derives an identical mask every time otherwise -- and it
/// is not cheap. Measured at UHD with a 20px feather it is ~50 ms per frame,
/// which was the single largest per-frame cost in the plugin.
///
/// The segmentation is keyed by POINTER, not by content. The analysis cache
/// above already guarantees that a given Segmentation object corresponds to one
/// content hash and one set of detect settings, so pointer identity is both
/// sufficient and free -- and holding the shared_ptr keeps the object alive, so
/// the address cannot be recycled underneath the comparison.
struct MaskCache {
    std::mutex mutex;
    std::shared_ptr<const Segmentation> seg;
    std::string points;
    MaskParams params;
    std::shared_ptr<const std::vector<float>> mask;
};

} // namespace am

/// The whole class is declared here, unlike the sibling plugins which only
/// declare their factory, because the interact needs CurrentSegmentation().
/// The alternative -- the interact fetching the source clip and labelling it
/// itself -- would run a full analysis on every mouse-move, and in a
/// GPU-resident host would be handed device pointers it cannot read.
class AutoMaskPlugin : public OFX::ImageEffect
{
public:
    explicit AutoMaskPlugin(OfxImageEffectHandle p_Handle);
    ~AutoMaskPlugin() override;

    void render(const OFX::RenderArguments& p_Args) override;
    void changedParam(const OFX::InstanceChangedArgs& p_Args,
                      const std::string& p_ParamName) override;

    /// The most recent analysis, or null if nothing has been rendered yet.
    /// Locks, bumps the refcount, and returns; the caller holds it safely for
    /// as long as it likes even if the next render replaces the cache entry.
    std::shared_ptr<const am::Segmentation> CurrentSegmentation() const;

    /// The anchor string, as the overlay reads and writes it.
    std::string SelectedPoints() const;
    void SetSelectedPoints(const std::string& s);

    bool ShowAllElements() const;

private:
    /// Reads the segmentation controls at @p time into a DetectParams.
    am::DetectParams DetectParamsAt(double time) const;

    /// Everything both render paths need, read once.
    ///
    /// Shared deliberately: two copies of "read the parameters and decide what
    /// they mean" is how a CPU and a GPU path start producing different
    /// pictures for reasons that have nothing to do with either kernel.
    struct FrameParams {
        am::DetectParams det;
        am::MaskParams mask;
        int outputMode = 0;
        int backgroundMode = 0;
        am::FilterMode filter = am::kFilterBilinear;
        double posX = 0.0, posY = 0.0;
        double scaleX = 1.0, scaleY = 1.0;
        double rotation = 0.0;
        double anchorX = 0.5, anchorY = 0.5;

        /// The composite parameters for a frame of this size.
        am::CompositeParams composite(float w, float h) const;
    };
    FrameParams ReadFrameParams(double time) const;

    /// Bounding box of everything currently selected, in top-down pixels.
    /// Returns false when nothing is selected or nothing has been analysed.
    bool SelectionBounds(double time, const am::Segmentation& seg, am::RectI& out) const;

    /// One place, one pass, for every visibility rule. Never called from render.
    void syncVisibility();

    /// Rewrites the read-only element count. Best effort; a host that refuses
    /// the write just shows a stale string.
    void updateStatus();

    OFX::Clip* _srcClip = nullptr;
    OFX::Clip* _dstClip = nullptr;

    OFX::ChoiceParam*  _segMode        = nullptr;
    OFX::DoubleParam*  _threshold      = nullptr;
    OFX::BooleanParam* _invertSource   = nullptr;
    OFX::IntParam*     _minElementSize = nullptr;
    OFX::IntParam*     _bridgeGaps     = nullptr;
    OFX::BooleanParam* _showAll        = nullptr;
    OFX::StringParam*  _elementStatus  = nullptr;

    OFX::StringParam*  _selectedPoints = nullptr;
    OFX::PushButtonParam* _selectAll      = nullptr;
    OFX::PushButtonParam* _clearSelection = nullptr;

    OFX::ChoiceParam*  _outputMode    = nullptr;
    OFX::BooleanParam* _leaveOriginal = nullptr;
    OFX::BooleanParam* _invertMask    = nullptr;
    OFX::IntParam*     _expand        = nullptr;
    OFX::DoubleParam*  _feather       = nullptr;

    OFX::Double2DParam* _xfPos    = nullptr;
    OFX::DoubleParam*   _xfScale  = nullptr;
    OFX::DoubleParam*   _xfScaleY = nullptr;
    OFX::BooleanParam*  _xfLink   = nullptr;
    OFX::DoubleParam*   _xfRot    = nullptr;
    OFX::Double2DParam* _xfAnchor = nullptr;
    OFX::PushButtonParam* _anchorToSel = nullptr;
    OFX::PushButtonParam* _autoCenter  = nullptr;
    OFX::ChoiceParam*   _filter   = nullptr;

    mutable am::AnalysisCache _cache;
    mutable am::MaskCache _maskCache;

#ifdef AUTOMASK_HAVE_CUDA
    /// Device-resident label image, selection lookup and built mask.
    ///
    /// Tracked with its own keys rather than reusing the host mask cache: the
    /// two can be valid independently, and a frame that hits on the host but
    /// misses on the device still has to re-upload.
    am::CudaContext* _cuda = nullptr;
    std::shared_ptr<const am::Segmentation> _cudaLabelsFor;
    std::shared_ptr<const am::Segmentation> _cudaMaskSeg;
    std::string _cudaMaskPoints;
    am::MaskParams _cudaMaskParams;
    bool _cudaMaskValid = false;
    std::vector<float> _downloadBuf;   ///< host staging for the analysis
    std::mutex _cudaMutex;

    /// Returns false if anything about the device path could not be serviced,
    /// which is fatal for the frame -- by then the pixel addresses are device
    /// memory and the CPU loop cannot touch them.
    bool renderCuda(const OFX::RenderArguments& args, OFX::Image* src, OFX::Image* dst);
#endif
};
