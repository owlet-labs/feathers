/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "device/endpoint.h"

// THE ENDS OF A ROUTE, AS THE BACKPLANE NAMES THEM — the device vocabulary's types (`device/endpoint.h`,
// where the argument is), one set of types under both names.

namespace feathers::backplane {

using device::Destination;
using device::Route;
using device::Source;
using device::Transfer;

} // namespace feathers::backplane
