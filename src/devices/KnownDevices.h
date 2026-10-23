// SPDX-License-Identifier: GPL-3.0-or-later
// Known capture hardware: IDs, the driver each needs, quirks, and per-chip
// pixel aspect. This is the test and quirk matrix, not a whitelist: any device
// whose driver exposes a V4L2 capture node works whether it is listed or not.
#pragma once

#include "engine/Types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pd {

struct KnownDevice {
    enum class Bus { USB, PCI };
    Bus bus = Bus::USB;
    uint16_t vendor = 0;
    uint16_t product = 0;     // 0xffff: any product of this vendor
    const char* name = "";
    const char* chip = "";
    const char* drivers = ""; // kernel driver names that bind it, comma separated
    bool mainline = true;
    const char* link = "";
    const char* notes = "";
    Rational sar{0, 1};       // chip override, 0/1 = derive from sampling
};

const KnownDevice* lookupKnownDevice(KnownDevice::Bus bus, uint16_t vendor, uint16_t product);
const std::vector<KnownDevice>& knownDevices();
// A sentence or three telling the user what driver the device needs and how to get it.
std::string missingDriverMessage(const KnownDevice& d);

}  // namespace pd
