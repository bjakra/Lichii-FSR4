// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>

#include <catch2/catch_test_macros.hpp>

#include "video_core/fsr4_support.h"

TEST_CASE("FSR 4 is offered on RDNA 2 and newer Radeons on AMD's driver", "[video_core][fsr4]") {
    using VideoCommon::IsFsr4CapableGpu;
    constexpr u32 amd = 0x1002;
    for (const char* name :
         {"AMD Radeon RX 9070 XT", "AMD Radeon RX 9060 XT", "AMD Radeon RX 7900 XTX",
          "AMD Radeon RX 7600S", "AMD Radeon RX 6800 XT", "AMD Radeon RX 6700M",
          "AMD Radeon PRO W7900", "AMD Radeon(TM) 890M Graphics", "AMD Radeon(TM) 880M",
          "AMD Radeon(TM) 8060S Graphics", "AMD Radeon 780M Graphics"}) {
        INFO(name);
        CHECK(IsFsr4CapableGpu(amd, true, name));
    }
    for (const char* name :
         {"AMD Radeon RX 5700 XT", "AMD Radeon RX 580", "AMD Radeon RX Vega", "AMD Radeon(TM) 680M",
          "AMD Radeon(TM) Graphics", "AMD Radeon VII", "AMD Radeon RX 590 GME"}) {
        INFO(name);
        CHECK_FALSE(IsFsr4CapableGpu(amd, true, name));
    }
    CHECK_FALSE(IsFsr4CapableGpu(amd, false, "AMD Radeon RX 9070 XT (RADV GFX1201)"));
    CHECK_FALSE(IsFsr4CapableGpu(0x10DE, true, "NVIDIA GeForce RTX 3080"));
    CHECK_FALSE(IsFsr4CapableGpu(0x8086, true, "Intel(R) Arc(TM) A770 Graphics"));
}

TEST_CASE("FSR 4 descriptor gate accepts enabled Vulkan 1.2 core buffer device address",
          "[video_core][fsr4]") {
    using VideoCommon::HasFsr4PortDescriptorFeatures;
    // VK_KHR_buffer_device_address may be absent because Vulkan 1.2 provides the core feature.
    CHECK(HasFsr4PortDescriptorFeatures(true, true, true));
    CHECK_FALSE(HasFsr4PortDescriptorFeatures(false, true, true));
    CHECK_FALSE(HasFsr4PortDescriptorFeatures(true, false, true));
    CHECK_FALSE(HasFsr4PortDescriptorFeatures(true, true, false));
}

TEST_CASE("The fsr4-vk FSR 4 port runs on RDNA 2 and newer Radeons on RADV with its features",
          "[video_core][fsr4]") {
    using VideoCommon::CheckFsr4Port;
    using VideoCommon::Fsr4PortDevice;
    using VideoCommon::Fsr4PortModel;
    constexpr Fsr4PortDevice rdna3{
        .vendor_id = 0x1002,
        .device_id = 0x744c, // Navi 31
        .radv = true,
        .mixed_dot4_accelerated = true,
        .descriptor_buffer = true,
        .synchronization2 = true,
        .compute_wave32 = true,
        .shader_arithmetic = true,
        .compute_derivatives = true,
        .fp8_matrices = false,
    };
    CHECK(CheckFsr4Port(rdna3).model == Fsr4PortModel::Int8);

    Fsr4PortDevice rdna4 = rdna3;
    rdna4.device_id = 0x7550; // Navi 48
    rdna4.fp8_matrices = true;
    CHECK(CheckFsr4Port(rdna4).model == Fsr4PortModel::Fp8);

    Fsr4PortDevice rdna2 = rdna3;
    rdna2.device_id = 0x73bf; // Navi 21
    rdna2.mixed_dot4_accelerated = false;
    CHECK(CheckFsr4Port(rdna2).model == Fsr4PortModel::Int8);
    rdna2.device_id = 0x1681; // Rembrandt
    CHECK(CheckFsr4Port(rdna2).model == Fsr4PortModel::Int8);

    Fsr4PortDevice rdna1 = rdna2;
    rdna1.device_id = 0x731f; // Navi 10
    CHECK_FALSE(CheckFsr4Port(rdna1).model);

    Fsr4PortDevice amdvlk = rdna4;
    amdvlk.radv = false;
    CHECK_FALSE(CheckFsr4Port(amdvlk).model);

    Fsr4PortDevice nvidia = rdna4;
    nvidia.vendor_id = 0x10DE;
    CHECK_FALSE(CheckFsr4Port(nvidia).model);

    for (bool Fsr4PortDevice::*feature :
         {&Fsr4PortDevice::descriptor_buffer, &Fsr4PortDevice::synchronization2,
          &Fsr4PortDevice::compute_wave32, &Fsr4PortDevice::shader_arithmetic,
          &Fsr4PortDevice::compute_derivatives}) {
        Fsr4PortDevice missing = rdna4;
        missing.*feature = false;
        const auto support = CheckFsr4Port(missing);
        CHECK_FALSE(support.model);
        CHECK_FALSE(support.reason.empty());
    }
}

TEST_CASE("FSR 4's output check tells a black or NaN output over a real picture",
          "[video_core][fsr4]") {
    using VideoCommon::CheckFsr4Output;
    using VideoCommon::Fsr4OutputCheck;
    using Texel = std::array<u16, 4>;
    constexpr u16 one = 0x3c00;
    constexpr u16 half = 0x3800;
    constexpr u16 nan = 0x7e00;
    constexpr u16 negative_zero = 0x8000;
    std::array<Texel, 64> varied{};
    for (size_t i = 0; i < varied.size(); ++i) {
        varied[i] = {static_cast<u16>(0x2000 + i * 0x80), half, half, one};
    }
    std::array<Texel, 64> black{};
    black.fill({0, negative_zero, 0, one}); // alpha doesn't count
    std::array<Texel, 64> not_finite{};
    not_finite.fill({nan, nan, nan, one});

    CHECK(CheckFsr4Output(varied, varied) == Fsr4OutputCheck::Picture);
    CHECK(CheckFsr4Output(varied, black) == Fsr4OutputCheck::Black);
    CHECK(CheckFsr4Output(varied, not_finite) == Fsr4OutputCheck::Black);

    // One real pixel is enough.
    std::array<Texel, 64> mostly_black = black;
    mostly_black[17] = {half, 0, 0, one};
    CHECK(CheckFsr4Output(varied, mostly_black) == Fsr4OutputCheck::Picture);

    // A flat input (a fade, a loading screen) can't be judged.
    std::array<Texel, 64> flat{};
    flat.fill({half, half, half, one});
    CHECK(CheckFsr4Output(flat, black) == Fsr4OutputCheck::Unjudged);
    std::array<Texel, 64> few_distinct = flat;
    for (size_t i = 1; i < 15; ++i) {
        few_distinct[i] = varied[i];
    }
    CHECK(CheckFsr4Output(few_distinct, black) == Fsr4OutputCheck::Unjudged);
    few_distinct[15] = varied[15];
    few_distinct[16] = varied[16];
    CHECK(CheckFsr4Output(few_distinct, black) == Fsr4OutputCheck::Black);
}

TEST_CASE("FSR 4 that couldn't start is tried again a few times, then given up",
          "[video_core][fsr4]") {
    using VideoCommon::Fsr4RetryDelay;
    CHECK_FALSE(Fsr4RetryDelay(0));
    CHECK(Fsr4RetryDelay(1) == 10u);
    CHECK(Fsr4RetryDelay(2) == 60u);
    CHECK_FALSE(Fsr4RetryDelay(3));
    CHECK_FALSE(Fsr4RetryDelay(100));
}
