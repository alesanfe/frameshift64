// SPDX-License-Identifier: MIT
#pragma once

struct WidgetInfo;

namespace ssb64 {

void RenderPostProcessLowResWarnModal();
// Declared unconditionally — Switch links a no-op body (no download service).
void RenderShaderPackModal();

/* Customization-panel renderers — defined in PortMenuCustomization.cpp,
 * wired into sidebar widgets from PortMenuCustomizationMods.cpp. */
void RenderPostProcessShaderPicker(WidgetInfo& widget);
void RenderPostProcessDiagnostics(WidgetInfo& widget);
void RenderShaderPackDownloader(WidgetInfo& widget);
void RenderCuratedTexturePacks(WidgetInfo& widget);

/* Support -> "Restart in Safe Mode" and the search action: drops a one-shot
 * safemode.request marker, relaunches the process so the boot gate sees it
 * (a soft console reset doesn't re-run startup gating). */
void RequestSafeModeRestart();

} // namespace ssb64
