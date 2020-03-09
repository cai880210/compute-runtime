/*
 * Copyright (C) 2019-2020 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 *
 */

#include "shared/source/gen9/hw_cmds.h"
#include "shared/source/gen9/hw_info.h"

#include "level_zero/core/source/sampler_hw.inl"

namespace L0 {

template <>
struct SamplerProductFamily<IGFX_SKYLAKE> : public SamplerCoreFamily<IGFX_GEN9_CORE> {
    using SamplerCoreFamily::SamplerCoreFamily;
};

static SamplerPopulateFactory<IGFX_SKYLAKE, SamplerProductFamily<IGFX_SKYLAKE>> populateSKL;

} // namespace L0