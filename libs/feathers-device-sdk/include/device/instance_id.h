/*
 * Copyright © 2026 Owlet Records
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the Feathers Device SDK, which is MIT-licensed (see LICENSE in the SDK's
 * root) so that devices of any licence, open or closed, may build against it. Feathers itself,
 * which vendors this SDK, is GPL-3.0-or-later.
 */

#pragma once

#include <cstdint>

// WHAT NAMES ONE DEVICE INSTANCE — declared in the device vocabulary since kill-lanes step 2.
//
// It was the backplane's (`backplane/device_instance_id.h`, which says why it exists at all and still
// includes this). It moved DOWN a layer when a route's ENDPOINTS became what a widget speaks: a
// `device::Modulatable` asks how far a SOURCE reaches it, a source names a device by this, and the device
// vocabulary may include nothing from the backplane (`test_composite_surface.cpp` reads the include graph).
// Unrenamed and in the same namespace, so every includer compiles unchanged.

namespace feathers {

using DeviceInstanceId = uint32_t;

/// Never handed out. A slot holding this holds nothing.
inline constexpr DeviceInstanceId kNoInstance = 0;

/// The first serial any counter hands out.
inline constexpr DeviceInstanceId kFirstInstance = 1;

} // namespace feathers
