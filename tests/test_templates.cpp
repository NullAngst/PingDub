// SPDX-License-Identifier: GPL-3.0-or-later
// Output folder templates: every token resolves, an empty label, a label
// with "/" and an unknown token each do what the README says, and Recover
// finds captures in template subfolders.
#include "engine/FolderTemplate.h"
#include "engine/SysInfo.h"
#include "testing.h"

#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace pd;
namespace fs = std::filesystem;

namespace {

int64_t at(int y, int mo, int d, int h, int mi, int s) {
    struct tm t {};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = s;
    t.tm_isdst = -1;
    return int64_t(mktime(&t)) * 1000000000LL;
}

TemplateValues sample() {
    TemplateValues v;
    v.title = "Wedding 1994";
    v.startRealNs = at(2026, 10, 25, 17, 48, 0);
    v.device = "gvusb2";
    v.input = "S-Video";
    v.profile = "VCR S-video NTSC";
    return v;
}

}  // namespace

TEST_CASE("every token resolves") {
    std::string out, err;
    CHECK(resolveFolderTemplate("/caps/{title}/{date}/{year}-{month}/{time}/{device}/{input}/{profile}", sample(), out, err));
    CHECK_MSG(out == "/caps/Wedding 1994/2026-10-25/2026-10/174800/gvusb2/S-Video/VCR S-video NTSC", "%s", out.c_str());
    CHECK(validateFolderTemplate("~/Videos/PingDub/{title}/", err));
    CHECK(templateUsesToken("~/Videos/PingDub/{title}/", "title"));
    CHECK(!templateUsesToken("~/Videos/PingDub/", "title"));
}

TEST_CASE("home is expanded and the trailing slash dropped") {
    std::string out, err;
    CHECK(resolveFolderTemplate("~/Videos/PingDub/{title}/", sample(), out, err));
    CHECK(out == homeDir() + "/Videos/PingDub/Wedding 1994");
}

TEST_CASE("empty label becomes Untitled") {
    TemplateValues v = sample();
    v.title = "";
    std::string out, err;
    CHECK(resolveFolderTemplate("/caps/{title}", v, out, err));
    CHECK(out == "/caps/Untitled");
    v.title = " .. ";  // nothing left after trimming
    CHECK(resolveFolderTemplate("/caps/{title}", v, out, err));
    CHECK(out == "/caps/Untitled");
}

TEST_CASE("slashes and control characters become underscores, values are trimmed and capped") {
    TemplateValues v = sample();
    v.title = "AC/DC live";
    std::string out, err;
    CHECK(resolveFolderTemplate("/caps/{title}", v, out, err));
    CHECK(out == "/caps/AC_DC live");
    v.title = std::string("  .tab\there\x01. ");
    CHECK(resolveFolderTemplate("/caps/{title}", v, out, err));
    CHECK_MSG(out == "/caps/tab_here_", "%s", out.c_str());
    v.title = std::string(150, 'x');
    CHECK(resolveFolderTemplate("/caps/{title}", v, out, err));
    CHECK_EQ(out.size(), std::string("/caps/").size() + 100);
    // A multi-byte character at the cut is not split.
    v.title = std::string(99, 'a') + "\xc3\xa9" + "zz";
    CHECK(cleanTemplateValue(v.title).size() == 99);
}

TEST_CASE("unknown and malformed tokens are errors") {
    std::string out, err;
    CHECK(!resolveFolderTemplate("/caps/{titel}", sample(), out, err));
    CHECK(err.find("{titel}") != std::string::npos);
    CHECK(!validateFolderTemplate("/caps/{title", err));
    CHECK(!validateFolderTemplate("/caps/title}", err));
    CHECK(!validateFolderTemplate("", err));
}

TEST_CASE("base folder and nearest existing parent") {
    CHECK(templateBase("/caps/{title}/{date}/") == "/caps");
    CHECK(templateBase("/caps/tape-{title}") == "/caps");
    CHECK(templateBase("/caps/plain/") == "/caps/plain");
    CHECK(templateBase("~/Videos/PingDub/{title}/") == homeDir() + "/Videos/PingDub");
    const std::string tmp = fs::temp_directory_path().string();
    CHECK(nearestExistingDir(tmp + "/pingdub-does-not-exist/a/b") == tmp);
}

TEST_CASE("recover finds captures in template subfolders") {
    char tmpl[] = "/tmp/pingdub-tmpl-XXXXXX";
    const std::string base = mkdtemp(tmpl);
    const std::string a = base + "/Wedding 1994", b = base + "/AC_DC live/2026-10-25";
    fs::create_directories(a);
    fs::create_directories(b);
    for (const std::string& f : {a + "/2026-10-25_174800_Wedding_1994.mkv", b + "/2026-10-25_190000_AC_DC_live.avi",
                                 a + "/2026-10-25_174800_Wedding_1994.recovered.mkv", a + "/notes.txt"}) {
        std::ofstream(f) << "x";
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const auto found = findCaptures(templateBase(base + "/{title}/"));
    CHECK_EQ(found.size(), size_t(2));
    if (found.size() == 2) {
        // Newest first.
        CHECK(found[0].find("AC_DC_live.avi") != std::string::npos);
        CHECK(found[1].find("Wedding_1994.mkv") != std::string::npos);
    }
    fs::remove_all(base);
}

TEST_MAIN()
