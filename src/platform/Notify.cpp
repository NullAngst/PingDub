// SPDX-License-Identifier: GPL-3.0-or-later
#include "Notify.h"

#include <dbus/dbus.h>

#include <mutex>

namespace pd {

bool sendDesktopNotification(const std::string& summary, const std::string& body, std::string& err, const std::string& icon) {
    static std::once_flag once;
    std::call_once(once, [] { dbus_threads_init_default(); });
    DBusError e;
    dbus_error_init(&e);
    DBusConnection* c = dbus_bus_get_private(DBUS_BUS_SESSION, &e);
    if (!c) {
        err = e.message ? e.message : "no session bus";
        dbus_error_free(&e);
        return false;
    }
    dbus_connection_set_exit_on_disconnect(c, FALSE);
    DBusMessage* m = dbus_message_new_method_call("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                                                  "org.freedesktop.Notifications", "Notify");
    bool ok = false;
    if (m) {
        DBusMessageIter it, arr, dict, entry, var;
        dbus_message_iter_init_append(m, &it);
        const char* app = "PingDub";
        const dbus_uint32_t replaces = 0;
        const char* iconS = icon.c_str();
        const char* sum = summary.c_str();
        const char* bod = body.c_str();
        const dbus_int32_t timeout = -1;
        dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &app);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &replaces);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iconS);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &sum);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &bod);
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &arr);
        dbus_message_iter_close_container(&it, &arr);
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
        {
            // The desktop file name lets KDE group it under PingDub and use its icon.
            dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
            const char* key = "desktop-entry";
            const char* val = "io.github.nullangst.PingDub";
            dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
            dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &var);
            dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &val);
            dbus_message_iter_close_container(&entry, &var);
            dbus_message_iter_close_container(&dict, &entry);
        }
        dbus_message_iter_close_container(&it, &dict);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &timeout);
        DBusMessage* r = dbus_connection_send_with_reply_and_block(c, m, 2000, &e);
        dbus_message_unref(m);
        if (r) {
            ok = true;
            dbus_message_unref(r);
        } else {
            err = e.message ? e.message : "the notification service did not answer";
            dbus_error_free(&e);
        }
    } else {
        err = "out of memory";
    }
    dbus_connection_close(c);
    dbus_connection_unref(c);
    return ok;
}

}  // namespace pd
