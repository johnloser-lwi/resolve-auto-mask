#pragma once

#include "ofxsImageEffect.h"

#include <memory>
#include <mutex>

#include "core/Segmentation.h"

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
    OFX::ChoiceParam*   _filter   = nullptr;

    mutable am::AnalysisCache _cache;
};
