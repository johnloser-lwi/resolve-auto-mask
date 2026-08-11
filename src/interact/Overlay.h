#pragma once

#include "ofxsImageEffect.h"
#include "ofxsInteract.h"

namespace am {

/** @brief Attach the viewer overlay to the effect descriptor.
 *
 * Must be called from describe(), NOT describeInContext(): the overlay
 * descriptor is a plugin-level property, and the host caches the descriptor, so
 * it would not necessarily pick the property up from the per-context pass.
 *
 * Remember that Resolve does not show OFX overlays at all until the on-screen
 * control dropdown under the Viewer is set to "Open FX Overlay". Nothing here
 * can work around that; it is a host-side switch, which is why the panel says
 * so in plain words.
 */
void RegisterOverlay(OFX::ImageEffectDescriptor& desc);

} // namespace am
