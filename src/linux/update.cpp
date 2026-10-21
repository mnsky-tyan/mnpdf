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
static const char* kRepoApi = "https://api.github.com/repos/mnsky-tyan/mnpdf/releases";

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
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ("mnpdf/" + std::string(gAppVersion)).c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    r.code = curl_easy_perform(curl);
    if (r.code == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.status);
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

// ---- the releases list -----------------------------------------------------
// A tiny scan for the fields the answer needs. The JSON is parsed by hand
// because the answer only needs the tag and the asset names, and a JSON library
// would be a dependency for two strings. There is deliberately no "prerelease"
// field: a Linux-only release IS a prerelease until its Windows archive ships,
// so skipping prereleases would skip exactly the releases this check exists to
// find.
struct ReleaseInfo {
    std::string tag;
    bool hasLinuxAsset = false;
};

static std::string jsonStringField(const std::string& json, const std::string& key, size_t from) {
    std::string needle = "\"" + key + "\"";
    size_t k = json.find(needle, from);
    if (k == std::string::npos) return "";
    size_t colon = json.find(':', k + needle.size());
    if (colon == std::string::npos) return "";
    size_t q1 = json.find('"', colon);
    if (q1 == std::string::npos) return "";
    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return json.substr(q1 + 1, q2 - q1 - 1);
}

static std::vector<ReleaseInfo> fetchReleases(std::string* why) {
    std::vector<ReleaseInfo> out;
    HttpReply r = httpGet(kRepoApi);
    if (r.status != 200 || r.body.empty()) {
        if (why) *why = httpFailureText(r.code, r.status);
        return out;
    }
    // one object per release: the list is a JSON array of objects
    size_t pos = 0;
    while (true) {
        size_t objStart = r.body.find('{', pos);
        if (objStart == std::string::npos) break;
        size_t objEnd = r.body.find("\n  }", objStart);
        std::string obj = (objEnd == std::string::npos) ? r.body.substr(objStart)
                                                        : r.body.substr(objStart, objEnd - objStart);
        ReleaseInfo ri;
        ri.tag = jsonStringField(obj, "tag_name", 0);
        // the Linux archive is named after the tag, exactly as the release
        // workflow publishes it
        std::string want = "mnpdf-linux-x64-" + ri.tag + ".tar.gz";
        ri.hasLinuxAsset = !ri.tag.empty() && obj.find(want) != std::string::npos;
        if (!ri.tag.empty()) out.push_back(ri);
        if (objEnd == std::string::npos) break;
        pos = objEnd + 1;
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
};

// The newest release that carries a Linux archive, prerelease or not: a Linux
// release is a prerelease until the Windows archive is published beside it, and
// the reader who installed it should still be told about the next one. The API
// lists releases newest first, so the first one with a Linux archive is the
// newest such release.
UpdateResult checkForUpdate() {
    UpdateResult res;
    std::string why;
    std::vector<ReleaseInfo> releases = fetchReleases(&why);
    if (releases.empty()) {
        res.error = "The update check could not reach GitHub - " + why + ".";
        return res;
    }
    res.ok = true;
    for (const ReleaseInfo& ri : releases) {
        if (!ri.hasLinuxAsset) continue;
        res.latestTag = ri.tag;
        break;
    }
    if (res.latestTag.empty()) {
        res.error = "No release with a Linux build was found.";
        return res;
    }
    res.newer = versionIsNewer(res.latestTag);
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
// clicking "Check for updates" twice in a row makes one request, not two.
bool updateCheckNow(std::string& latestTag, bool& newer, std::string& error) {
    if (updateCheckCooling()) {
        error = "The update check already ran less than an hour ago.";
        return false;
    }
    if (!updateCheckBlocking(latestTag, newer, error)) {
        noteUpdateCheck("");     // a failed attempt still counts
        return false;
    }
    noteUpdateCheck(latestTag);
    return true;
}
