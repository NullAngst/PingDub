// SPDX-License-Identifier: GPL-3.0-or-later
#include "RtKit.h"

#include <dbus/dbus.h>

#include <algorithm>
#include <mutex>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace pd {

namespace {

const char* kService = "org.freedesktop.RealtimeKit1";
const char* kPath = "/org/freedesktop/RealtimeKit1";

bool getProperty(DBusConnection* c, const char* name, long long& value, std::string& err) {
    DBusMessage* m = dbus_message_new_method_call(kService, kPath, "org.freedesktop.DBus.Properties", "Get");
    if (!m) return false;
    const char* iface = kService;
    dbus_message_append_args(m, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
    DBusError e;
    dbus_error_init(&e);
    DBusMessage* r = dbus_connection_send_with_reply_and_block(c, m, 2000, &e);
    dbus_message_unref(m);
    if (!r) {
        err = e.message ? e.message : "rtkit not reachable";
        dbus_error_free(&e);
        return false;
    }
    DBusMessageIter it, sub;
    bool ok = false;
    if (dbus_message_iter_init(r, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(&it, &sub);
        const int t = dbus_message_iter_get_arg_type(&sub);
        if (t == DBUS_TYPE_INT32) {
            dbus_int32_t v;
            dbus_message_iter_get_basic(&sub, &v);
            value = v;
            ok = true;
        } else if (t == DBUS_TYPE_INT64) {
            dbus_int64_t v;
            dbus_message_iter_get_basic(&sub, &v);
            value = v;
            ok = true;
        }
    }
    dbus_message_unref(r);
    return ok;
}

}  // namespace

bool makeCurrentThreadRealtime(int priority, std::string& err) {
    static std::once_flag once;
    std::call_once(once, [] { dbus_threads_init_default(); });
    DBusError e;
    dbus_error_init(&e);
    DBusConnection* c = dbus_bus_get_private(DBUS_BUS_SYSTEM, &e);
    if (!c) {
        err = e.message ? e.message : "no system bus";
        dbus_error_free(&e);
        return false;
    }
    dbus_connection_set_exit_on_disconnect(c, FALSE);

    // rtkit refuses threads without an RLIMIT_RTTIME, so a runaway realtime
    // thread cannot lock up the desktop.
    long long rttime = 200000;
    getProperty(c, "RTTimeUSecMax", rttime, err);
    rlimit rl{rlim_t(rttime), rlim_t(rttime)};
    setrlimit(RLIMIT_RTTIME, &rl);
    long long maxPrio = 20;
    getProperty(c, "MaxRealtimePriority", maxPrio, err);
    const dbus_uint32_t prio = dbus_uint32_t(std::clamp<long long>(priority, 1, maxPrio));
    const dbus_uint64_t tid = dbus_uint64_t(syscall(SYS_gettid));

    DBusMessage* m = dbus_message_new_method_call(kService, kPath, kService, "MakeThreadRealtime");
    dbus_message_append_args(m, DBUS_TYPE_UINT64, &tid, DBUS_TYPE_UINT32, &prio, DBUS_TYPE_INVALID);
    DBusMessage* r = dbus_connection_send_with_reply_and_block(c, m, 2000, &e);
    dbus_message_unref(m);
    bool ok = r != nullptr;
    if (!ok) {
        err = e.message ? e.message : "rtkit refused";
        dbus_error_free(&e);
    } else {
        dbus_message_unref(r);
        err.clear();
    }
    dbus_connection_close(c);
    dbus_connection_unref(c);
    return ok;
}

}  // namespace pd
