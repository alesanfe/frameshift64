// SPDX-License-Identifier: MIT
#pragma once

namespace ssb64 {

void RenderPostProcessLowResWarnModal();
// Declared unconditionally — Switch links a no-op body (no download service).
void RenderShaderPackModal();

/* Support -> "Restart in Safe Mode" and the search action: drops a one-shot
 * safemode.request marker, relaunches the process so the boot gate sees it
 * (a soft console reset doesn't re-run startup gating). */
void RequestSafeModeRestart();

} // namespace ssb64
