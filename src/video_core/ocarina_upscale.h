// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cmath>
#include <string_view>

#include "common/common_types.h"

namespace VideoCommon {

enum class OcarinaUpscaleBackend { Native, Fsr, Dlss };

/// render_scaled: a DLSS render-scale patch shrank the scene to a size FSR 3.1 takes, so FSR 3.1
/// reconstructs it when DLSS can't run.
constexpr OcarinaUpscaleBackend SelectOcarinaUpscaler(bool fsr_enabled, bool dlss_enabled,
                                                    bool dlss_available, bool render_scaled,
                                                    std::string_view override) {
    if (override == "native") {
        return OcarinaUpscaleBackend::Native;
    }
    if (override == "fsr") {
        return OcarinaUpscaleBackend::Fsr;
    }
    if ((dlss_enabled || override == "dlss") && dlss_available) {
        return OcarinaUpscaleBackend::Dlss;
    }
    return fsr_enabled || render_scaled ? OcarinaUpscaleBackend::Fsr
                                        : OcarinaUpscaleBackend::Native;
}

/// The scene's height over FSR's output height, docked. With Max Resolution the game renders
/// render_height and FSR writes 3840x2160 or, keeping the game's output, 2560x1440; without it the
/// game's 3/4 pass takes 1080p to 1440p.
constexpr float OcarinaFsrRenderRatio(bool max_resolution, u32 render_height,
                                      bool keep_game_output) {
    if (!max_resolution) {
        return 0.75f;
    }
    return static_cast<float>(render_height) / (keep_game_output ? 1440.0f : 2160.0f);
}

/// Texture LOD bias added to Ocarina's mipmapped samplers while FSR upscales. AMD recommends
/// log2(render / output) - 1: -1.58 for 1440p -> 4K, -1 for native anti-aliasing.
inline float OcarinaFsrLodBias(OcarinaUpscaleBackend backend, float render_ratio) {
    return backend == OcarinaUpscaleBackend::Fsr ? std::log2(render_ratio) - 1.0f : 0.0f;
}

/// Whether a sampler with that bias keeps it for an image: only docked (handheld isn't measured),
/// not for images the GPU wrote (render targets would read a mip too sharp), and not once the
/// upscaler failed this session (the game's own pass runs again).
constexpr bool KeepOcarinaFsrLodBias(bool docked, bool upscaler_failed, bool gpu_written) {
    return docked && !upscaler_failed && !gpu_written;
}

} // namespace VideoCommon
