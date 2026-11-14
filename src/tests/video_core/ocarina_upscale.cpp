// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include "video_core/ocarina_upscale.h"
#include "video_core/fsr.h"

TEST_CASE("OoT temporal backend selection is opt-in and vendor neutral", "[video_core][ocarina]") {
    using enum VideoCommon::OcarinaUpscaleBackend;
    using VideoCommon::SelectOcarinaUpscaler;
    CHECK(SelectOcarinaUpscaler(false, false, false, false, "") == Native);
    CHECK(SelectOcarinaUpscaler(false, false, true, false, "") == Native);
    CHECK(SelectOcarinaUpscaler(true, false, false, false, "") == Fsr);
    CHECK(SelectOcarinaUpscaler(true, false, true, false, "") == Fsr);
    CHECK(SelectOcarinaUpscaler(false, true, false, false, "") == Native);
    CHECK(SelectOcarinaUpscaler(false, true, true, false, "") == Dlss);
    CHECK(SelectOcarinaUpscaler(true, true, false, false, "") == Fsr);
    CHECK(SelectOcarinaUpscaler(true, true, true, false, "") == Dlss);
    CHECK(SelectOcarinaUpscaler(true, true, true, false, "native") == Native);
    CHECK(SelectOcarinaUpscaler(false, true, true, false, "fsr") == Fsr);
    CHECK(SelectOcarinaUpscaler(false, false, false, false, "fsr") == Fsr);
    CHECK(SelectOcarinaUpscaler(false, false, false, false, "dlss") == Native);
    CHECK(SelectOcarinaUpscaler(false, false, true, false, "dlss") == Dlss);
    CHECK(SelectOcarinaUpscaler(false, true, true, false, "dlss") == Dlss);
    CHECK(SelectOcarinaUpscaler(false, false, false, false, "garbage") == Native);
    // The DLSS render scale shrank the scene: FSR 3.1 reconstructs it when DLSS can't run.
    CHECK(SelectOcarinaUpscaler(false, true, false, true, "") == Fsr);
    CHECK(SelectOcarinaUpscaler(false, true, true, true, "") == Dlss);
    CHECK(SelectOcarinaUpscaler(false, true, false, true, "native") == Native);
}

TEST_CASE("OoT FSR texture LOD bias follows the upscaler, the console mode and failure",
          "[video_core][ocarina]") {
    using enum VideoCommon::OcarinaUpscaleBackend;
    using VideoCommon::KeepOcarinaFsrLodBias;
    using VideoCommon::OcarinaFsrLodBias;
    using VideoCommon::OcarinaFsrRenderRatio;
    // Max Resolution: 1440p, 1080p and 720p to 4K; 1440p, 960p and 720p to the game's 1440p.
    CHECK(OcarinaFsrLodBias(Fsr, OcarinaFsrRenderRatio(true, 1440, false)) ==
          Catch::Approx(-1.585f).margin(0.001f));
    CHECK(OcarinaFsrLodBias(Fsr, OcarinaFsrRenderRatio(true, 1080, false)) == -2.0f);
    CHECK(OcarinaFsrLodBias(Fsr, OcarinaFsrRenderRatio(true, 720, false)) ==
          Catch::Approx(-2.585f).margin(0.001f));
    CHECK(OcarinaFsrLodBias(Fsr, OcarinaFsrRenderRatio(true, 1440, true)) == -1.0f);
    CHECK(OcarinaFsrLodBias(Fsr, OcarinaFsrRenderRatio(true, 960, true)) ==
          Catch::Approx(-1.585f).margin(0.001f));
    CHECK(OcarinaFsrLodBias(Fsr, OcarinaFsrRenderRatio(true, 720, true)) == -2.0f);
    // Without Max Resolution the game's 3/4 pass takes 1080p to 1440p, whatever is chosen.
    CHECK(OcarinaFsrLodBias(Fsr, OcarinaFsrRenderRatio(false, 720, true)) ==
          Catch::Approx(-1.415f).margin(0.001f));
    CHECK(OcarinaFsrLodBias(Dlss, 0.5f) == 0.0f);
    CHECK(OcarinaFsrLodBias(Native, 0.5f) == 0.0f);
    CHECK(KeepOcarinaFsrLodBias(true, false, false));
    CHECK_FALSE(KeepOcarinaFsrLodBias(false, false, false));
    CHECK_FALSE(KeepOcarinaFsrLodBias(true, false, true));
    CHECK_FALSE(KeepOcarinaFsrLodBias(true, true, false));
    CHECK_FALSE(KeepOcarinaFsrLodBias(true, true, true));
}

TEST_CASE("FSR temporal sharpness uses the existing stops slider", "[video_core][fsr][ocarina]") {
    CHECK(FSR::RcasSharpnessFromStops(0.0f) == 1.0f);
    CHECK(FSR::RcasSharpnessFromStops(0.81f) == 0.595f);
    CHECK(FSR::RcasSharpnessFromStops(1.0f) == 0.5f);
    CHECK(FSR::RcasSharpnessFromStops(2.0f) == 0.0f);
    CHECK(FSR::RcasSharpnessFromStops(3.0f) == 0.0f);
    CHECK(FSR::RcasSharpnessFromStops(-1.0f) == 1.0f);
}
