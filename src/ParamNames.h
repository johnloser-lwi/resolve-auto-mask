#pragma once

// Every parameter name in one place.
//
// The names are the plugin's on-disk contract: they go into project files and
// into saved settings, so renaming one silently orphans every clip already set
// up with it. Keeping them here rather than as string literals at each use site
// means a rename is a compile error rather than a mismatch nobody notices until
// a project reopens with a control at its default.

namespace am {

// --- Segmentation ------------------------------------------------------------
constexpr const char* kParamSegMode        = "segMode";
constexpr const char* kParamThreshold      = "threshold";
constexpr const char* kParamInvertSource   = "invertSource";
constexpr const char* kParamMinElementSize = "minElementSize";
constexpr const char* kParamBridgeGaps     = "bridgeGaps";
constexpr const char* kParamShowAll        = "showAllElements";
constexpr const char* kParamElementStatus  = "elementStatus";

// --- Selection ---------------------------------------------------------------
constexpr const char* kParamSelectedPoints = "selectedPoints";
constexpr const char* kParamSelectAll      = "selectAll";
constexpr const char* kParamClearSelection = "clearSelection";

// --- Mask --------------------------------------------------------------------
constexpr const char* kParamOutputMode     = "outputMode";
constexpr const char* kParamLeaveOriginal  = "leaveOriginal";
constexpr const char* kParamInvertMask     = "invertMask";
constexpr const char* kParamExpand         = "expand";
constexpr const char* kParamFeather        = "feather";

// --- Transform ---------------------------------------------------------------
constexpr const char* kParamXfPos          = "xformPosition";
constexpr const char* kParamXfScale        = "xformScale";
constexpr const char* kParamXfScaleY       = "xformScaleY";
constexpr const char* kParamXfLink         = "xformScaleLink";
constexpr const char* kParamXfRot          = "xformRotation";
constexpr const char* kParamXfAnchor       = "xformAnchor";
constexpr const char* kParamAnchorToSel    = "anchorToSelection";
constexpr const char* kParamAutoCenter     = "autoCenter";
constexpr const char* kParamFilter         = "filterMode";

// --- Arrange -----------------------------------------------------------------
constexpr const char* kParamArrangeMode    = "arrangeMode";
constexpr const char* kParamArrangeOrder   = "arrangeOrder";
constexpr const char* kParamArrangeReverse = "arrangeReverse";
constexpr const char* kParamSpread         = "arrangeSpread";
constexpr const char* kParamStep           = "arrangeStep";
constexpr const char* kParamAlignH         = "arrangeAlignH";
constexpr const char* kParamAlignV         = "arrangeAlignV";

// --- About -------------------------------------------------------------------
constexpr const char* kParamOverlayHint    = "overlayHint";

// Choice indices. Written out rather than inferred from the option order,
// because the order is what the host stores: inserting an option in the middle
// would otherwise reinterpret every project already using this plugin.
enum { kSegAlpha = 0, kSegLuminance = 1 };
enum { kFilterOptNearest = 0, kFilterOptBilinear = 1 };

} // namespace am
