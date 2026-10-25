// SPDX-License-Identifier: GPL-3.0-or-later
// Output folder templates: "~/Videos/PingDub/{title}/" puts each tape's
// capture and sidecars in a folder named after its tape label. The template
// is resolved once, when a capture starts, so a capture never splits across
// folders.
//
// Tokens: {title} {date} {year} {month} {time} {device} {input} {profile}.
// Values have "/" and control characters replaced by "_", leading and
// trailing spaces and dots trimmed, and are capped at 100 characters. An
// empty tape label becomes "Untitled". Unknown tokens are an error.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pd {

struct TemplateValues {
    std::string title;     // tape label
    int64_t startRealNs = 0;
    std::string device;
    std::string input;
    std::string profile;
};

const std::vector<std::string>& templateTokens();  // "title", "date", ...

// True when the template is well formed and uses only known tokens.
bool validateFolderTemplate(const std::string& tmpl, std::string& err);
bool templateUsesToken(const std::string& tmpl, const std::string& token);
// One token value, cleaned up as described above.
std::string cleanTemplateValue(const std::string& v);
// Resolves the tokens and expands "~". Returns false for a bad template.
bool resolveFolderTemplate(const std::string& tmpl, const TemplateValues& v, std::string& out, std::string& err);
// The part before the first token, "~" expanded, without a trailing slash.
std::string templateBase(const std::string& tmpl);
// The folder itself if it exists, else its nearest existing parent.
std::string nearestExistingDir(const std::string& path);
// Every capture (.mkv or .avi) in `base` and every folder below it, newest
// first. Recovered copies are left out.
std::vector<std::string> findCaptures(const std::string& base, size_t limit = 500);

}  // namespace pd
