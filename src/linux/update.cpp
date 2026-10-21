// mnpdf for Linux - the update check.
//
// The Windows build finds updates through GitHub's /releases/latest and then
// downloads mnpdf-win-x64-<tag>.zip by name (src/main.cpp:2572). The Linux
// build cannot use that endpoint for two reasons: /releases/latest answers with
// the newest NON-prerelease, and a Linux-only release is a prerelease until the
// Windows archive is published alongside it; and the asset it would fetch is
// the Windows one. So this asks the releases API for the list and picks the
// newest release that actually carries a Linux archive, then compares versions
// the same way the Windows build does.
//
// The version this build reports is generated at compile time from the
// Windows kAppVersion constant (see CMakeLists.txt), so the two readers can
// never claim different versions - which matters, because they share one
// app.txt.
#include "doc.h"
#include "ui.h"
#include "mnpdf-version.h"

#include <curl/curl.h>

#include <ctime>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// gAppVersion is defined in the generated mnpdf-version.cpp
//
// The tag is read off the WEB host, never api.github.com, for the reason the
// Windows build gives (src/main.cpp:2430-2432): that host answers "API rate
// limit exceeded" once the IP's unauthenticated quota is spent, and that quota
// is shared with every other unauthenticated client behind the same IP. The
// web host has no such quota.
//
// The one thing the web host does not answer is the ASSET LIST, so the Linux
// archive is looked up on the release page itself: the page names the archive
// in its download links.
static const char* kGitHubRoot = "https://github.com";
static const char* kRepoPath = "mnsky-tyan/mnpdf";
static const std::string kLatestReleaseUrl = std::string(kGitHubRoot) + "/" + kRepoPath + "/releases/latest";
static const std::string kReleasePageUrl = std::string(kGitHubRoot) + "/" + kRepoPath + "/releases";

// ---- version helpers (the Windows rules, src/main.cpp:2334) ----------------
bool parseVersion(const std::string& text, int& major, int& minor, int& patch) {
    const char* p = text.c_str();
    if (*p == 'v' || *p == 'V') p++;
    return sscanf(p, "%d.%d.%d", &major, &minor, &patch) == 3;
}

bool versionIsNewer(const std::string& candidate) {
    int a = 0, b = 0, c = 0, x = 0, y = 0, z = 0;
    if (!parseVersion(gAppVersion, a, b, c)) return false;
    if (!parseVersion(candidate, x, y, z)) return false;
    if (x != a) return x > a;
    if (y != b) return y > b;
    return z > c;
}

// ---- the network -----------------------------------------------------------
// A small curl wrapper: the request is bounded by a timeout so a hung network
// can never leave the app waiting, and the answer is capped so a huge body
// cannot exhaust memory.
struct HttpReply {
    std::string body;
    std::string location;        // the Location header, when the answer is a redirect
    long status = 0;
    CURLcode code = CURLE_OK;   // the transport result, for the failure text
};

static size_t writeToString(void* ptr, size_t size, size_t nmemb, void* userdata) {
    std::string* out = (std::string*)userdata;
    if (out->size() > 4 * 1024 * 1024) return 0;       // 4 MB is far more than the answer needs
    out->append((const char*)ptr, size * nmemb);
    return size * nmemb;
}

static HttpReply httpGet(const std::string& url) {
    HttpReply r;
    CURL* curl = curl_easy_init();
    if (!curl) return r;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);
    // the redirect is NOT followed: the Windows build reads the Location
    // header instead, because that is where the release tag lives
    // (src/main.cpp:2466-2479). Following it would throw the tag away.
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ("mnpdf/" + std::string(gAppVersion)).c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    r.code = curl_easy_perform(curl);
    if (r.code == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.status);
        char* loc = nullptr;
        if (curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &loc) == CURLE_OK && loc)
            r.location = loc;
    }
    curl_easy_cleanup(curl);
    return r;
}

// The Linux counterpart of the Windows httpFailureText() (src/main.cpp:2594):
// name the failure the reader can act on instead of one generic sentence for a
// missing host, a refused connection and a timeout alike.
static std::string httpFailureText(CURLcode code, long status) {
    switch (code) {
    case CURLE_COULDNT_RESOLVE_HOST: return "the host name could not be resolved";
    case CURLE_COULDNT_CONNECT:      return "the connection was refused or dropped";
    case CURLE_OPERATION_TIMEDOUT:   return "the connection timed out";
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PEER_FAILED_VERIFICATION:
                                     return "the secure connection to the host failed";
    default: break;
    }
    if (code != CURLE_OK) return curl_easy_strerror(code);
    if (status) return "the server answered HTTP " + std::to_string(status);
    return "the server did not answer";
}

// ---- finding the release --------------------------------------------------
// Two requests, both to the web host (never the API - see the note above):
//
//  1. /releases/latest answers with a redirect to /releases/tag/<tag>. The
//     Windows build reads the Location header for exactly this reason
//     (src/main.cpp:2466-2479) and then validates the tag: only digits,
//     letters, dot, dash and underscore pass, because the tag becomes a URL
//     path component and a file name.
//
//  2. The release page names its assets in its download links, so the Linux
//     archive is found there. /releases/latest skips prereleases, and a
//     Linux-only release IS a prerelease until its Windows archive ships, so
//     the releases LIST is read as well and the newest release carrying a
//     Linux archive is taken from it.
//
// The tag is never taken on trust: it is validated the same way the Windows
// build validates it.
struct ReleaseInfo {
    std::string tag;
    bool hasLinuxAsset = false;
};

// a tag is only digits, letters, dot, dash and underscore - the same set the
// Windows build accepts before it turns a tag into a path (src/main.cpp:2481)
static bool cleanTag(const std::string& tag) {
    if (tag.empty()) return false;
    for (char ch : tag) {
        bool ok = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z')
               || (ch >= 'A' && ch <= 'Z') || ch == '.' || ch == '-' || ch == '_';
        if (!ok) return false;
    }
    return true;
}

// the tag out of a /releases/tag/<tag> URL, validated
static std::string tagFromUrl(const std::string& url) {
    const std::string marker = "/releases/tag/";
    size_t at = url.rfind(marker);
    if (at == std::string::npos) return "";
    std::string tag = url.substr(at + marker.size());
    // a trailing slash or a query string is not part of the tag
    while (!tag.empty() && (tag.back() == '/' || tag.back() == '?')) tag.pop_back();
    size_t q = tag.find('?');
    if (q != std::string::npos) tag = tag.substr(0, q);
    int a = 0, b = 0, c = 0;
    if (!cleanTag(tag) || !parseVersion(tag, a, b, c)) return "";
    return tag;
}

// The tag of the newest release, from the redirect /releases/latest answers
// with. An empty answer means the host did not redirect, which is a failure
// the caller reports rather than a release.
static std::string latestReleaseTag(std::string* why) {
    HttpReply r = httpGet(kLatestReleaseUrl);
    if (r.code != CURLE_OK) {
        if (why) *why = httpFailureText(r.code, r.status);
        return "";
    }
    if (r.status != 301 && r.status != 302 && r.status != 303 && r.status != 307 && r.status != 308) {
        if (why) *why = "the release host answered HTTP " + std::to_string(r.status);
        return "";
    }
    std::string target = r.location;
    if (target.empty()) {
        if (why) *why = "the release host did not say where the latest release is";
        return "";
    }
    // a site-relative redirect is made absolute; a redirect to some other host
    // is refused, the way the Windows build refuses it (src/main.cpp:2474)
    if (!target.empty() && target[0] == '/') target = std::string(kGitHubRoot) + target;
    else if (target.rfind(kGitHubRoot, 0) != 0) {
        if (why) *why = "the release host redirected somewhere else";
        return "";
    }
    std::string tag = tagFromUrl(target);
    if (tag.empty() && why) *why = "the release host named a release this build cannot read";
    return tag;
}

// The releases page, read for the newest release that carries a Linux
// archive. The page names each release and its assets, so a scan for the
// archive name and the tag beside it is enough.
static std::vector<ReleaseInfo> fetchReleases(std::string* why) {
    std::vector<ReleaseInfo> out;
    HttpReply r = httpGet(kReleasePageUrl);
    if (r.code != CURLE_OK || r.status != 200 || r.body.empty()) {
        if (why) *why = httpFailureText(r.code, r.status);
        return out;
    }
    // The page lists the releases newest first, and each release's block names
    // its tag and its assets. The Linux archive is named after the tag, so the
    // archive name in the block gives the tag back.
    const std::string kAssetPrefix = "mnpdf-linux-x64-";
    size_t pos = 0;
    while (true) {
        size_t asset = r.body.find(kAssetPrefix, pos);
        if (asset == std::string::npos) break;
        size_t nameEnd = r.body.find(".tar.gz", asset);
        if (nameEnd == std::string::npos) break;
        std::string tag = r.body.substr(asset + kAssetPrefix.size(),
                                        nameEnd - (asset + kAssetPrefix.size()));
        // the tag in the asset name is the tag of the release
        if (!tag.empty() && cleanTag(tag)) {
            ReleaseInfo ri;
            ri.tag = tag;
            ri.hasLinuxAsset = true;
            out.push_back(ri);
        }
        pos = nameEnd;
    }
    return out;
}

// ---- the cooldown ----------------------------------------------------------
// The Windows build asks GitHub at most once an hour (kUpdateCheckIntervalMin,
// src/main.cpp:67) and stamps the attempt - hit or miss - into app.txt, so
// neither a launch nor a menu click can burn the API's unauthenticated quota.
static const int kUpdateCheckIntervalMin = 60;

static long long nowSeconds() { return (long long)time(nullptr); }

// minutes since the last attempt, or -1 when there was none (or the clock
// moved back)
static int minutesSinceLastCheck() {
    if (gLastUpdateCheck <= 0) return -1;
    long long dt = nowSeconds() - gLastUpdateCheck;
    if (dt < 0) return -1;
    return (int)(dt / 60);
}

bool updateCheckCooling() {
    int m = minutesSinceLastCheck();
    return m >= 0 && m < kUpdateCheckIntervalMin;
}

// one attempt (hit or miss) stamps the clock, so the interval holds for the
// next request whatever the outcome was
void noteUpdateCheck(const std::string& tag) {
    gLastUpdateCheck = nowSeconds();
    if (!tag.empty()) gLastUpdateTag = tag;
    writeAppPref();
}

// ---- the check -------------------------------------------------------------
struct UpdateResult {
    bool ok = false;              // the check itself worked
    std::string error;            // why it did not
    std::string latestTag;        // the newest release carrying a Linux archive
    bool newer = false;           // and it is newer than this build
    std::string currentTag;       // the release GitHub calls current
    bool currentIsNewer = false;  // and it is newer than this build
};

// The newest release that carries a Linux archive, prerelease or not: a Linux
// release is a prerelease until the Windows archive is published beside it, and
// the reader who installed it should still be told about the next one. The API
// lists releases newest first, so the first one with a Linux archive is the
// newest such release.
UpdateResult checkForUpdate() {
    UpdateResult res;
    std::string why;

    // The releases page carries the newest release that has a Linux archive.
    // The page lists them newest first, so the first one is the answer.
    std::vector<ReleaseInfo> releases = fetchReleases(&why);
    if (releases.empty()) {
        res.error = "The update check could not reach GitHub - " + why + ".";
        return res;
    }
    for (const ReleaseInfo& ri : releases) {
        if (!ri.hasLinuxAsset) continue;
        res.latestTag = ri.tag;
        break;
    }
    if (res.latestTag.empty()) {
        res.error = "No release with a Linux build was found.";
        return res;
    }

    // The redirect of /releases/latest names the newest release GitHub
    // considers current - the same source the Windows build reads
    // (src/main.cpp:2435). It is not the answer here, because a Linux-only
    // release is a prerelease until its Windows archive ships and
    // /releases/latest skips prereleases. It is used to say so: a reader who
    // is behind the current release is told about the current one too.
    std::string currentTag = latestReleaseTag(&why);
    res.currentTag = currentTag;

    res.ok = true;
    res.newer = versionIsNewer(res.latestTag);
    res.currentIsNewer = !currentTag.empty() && versionIsNewer(currentTag);
    return res;
}

// The blocking half, called on a worker thread: it reports the newest release
// that carries a Linux archive and whether it is newer than this build.
bool updateCheckBlocking(std::string& latestTag, bool& newer, std::string& error) {
    UpdateResult r = checkForUpdate();
    if (!r.ok) {
        error = r.error;
        return false;
    }
    latestTag = r.latestTag;
    newer = r.newer;
    return true;
}

// The command's half: it honours the cooldown and stamps the attempt, so
// clicking "Check for updates" twice in a row makes one request, not two. It
// also reports the release GitHub calls current, which is a different release
// from the newest one carrying a Linux archive.
bool updateCheckNow(std::string& latestTag, bool& newer, std::string& error,
                    std::string& currentTag, bool& currentIsNewer) {
    if (updateCheckCooling()) {
        error = "The update check already ran less than an hour ago.";
        return false;
    }
    UpdateResult r = checkForUpdate();
    if (!r.ok) {
        error = r.error;
        noteUpdateCheck("");     // a failed attempt still counts
        return false;
    }
    latestTag = r.latestTag;
    newer = r.newer;
    currentTag = r.currentTag;
    currentIsNewer = r.currentIsNewer;
    noteUpdateCheck(latestTag);
    return true;
}

