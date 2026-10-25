// SPDX-License-Identifier: GPL-3.0-or-later
// Desktop notifications through the freedesktop notification service on the
// session bus (org.freedesktop.Notifications). KDE, GNOME and most other
// desktops show these as normal notifications. Uses libdbus, which PingDub
// already links for realtime priority.
#pragma once

#include <string>

namespace pd {

// Fire and forget. Returns false with a reason if the bus or the service
// could not be reached.
bool sendDesktopNotification(const std::string& summary, const std::string& body, std::string& err,
                             const std::string& icon = "io.github.nullangst.PingDub");

}  // namespace pd
