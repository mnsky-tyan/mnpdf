// mnpdf for Linux - the update check.
//
// The Windows build finds updates through GitHub's /releases/latest and then
// downloads mnpdf-win-x64-<tag>.zip by name (src/main.cpp:2572). The Linux
// build cannot use that endpoint for two reasons: /releases/latest answers with
// the newest NON-prerelease, and a Linux-only release is a prerelease until the
// Windows archive is published alongside it; and the asset it would fetch is
// the Windows one. So this reads the release feed, finds the newest release
// that actually carries a Linux archive, and compares versions the same way
// the Windows build does.
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
// The web host does not answer with an asset list, so the Linux archive is
// looked up the way a reader would: by asking for its download URL. The
// releases page itself is no help - it renders the asset list lazily, so the
// page's HTML carries the release NOTES but not the assets, and a scan of it
// for the archive name reads the notes rather than the file list.
static const char* kGitHubRoot = "https://github.com";
static const char* kRepoPath = "mnsky-tyan/mnpdf";
static const std::string kLatestReleaseUrl = std::string(kGitHubRoot) + "/" + kRepoPath + "/releases/latest";
static const std::string kReleaseFeedUrl = std::string(kGitHubRoot) + "/" + kRepoPath + "/releases.atom";

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
// libcurl's own contract: curl_global_init() is invoked exactly once per
// application, before any easy handle is created. Skipping it is not a crash
// on every platform - the library initialises itself lazily in most builds -
// but it is undefined behaviour that a different libcurl build can answer
// with, so the call is made once, here, the first time the network is used.
static void ensureCurlGlobal() {
    static bool done = false;
    if (done) return;
    done = true;
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

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

// One request, two shapes. A GET reads the body; a HEAD asks for the status
// alone, which is how the asset check avoids downloading the file it is only
// asking about.
static HttpReply httpRequest(const std::string& url, bool head) {
    HttpReply r;
    ensureCurlGlobal();
    CURL* curl = curl_easy_init();
    if (!curl) return r;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    if (head) {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    } else {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);
    }
    // the redirect is NOT followed: the Windows build reads the Location
    // header instead, because that is where the release tag lives
    // (src/main.cpp:2466-2479). Following it would throw the tag away, and
    // the asset check wants the redirect itself as the answer.
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

static HttpReply httpGet(const std::string& url) { return httpRequest(url, false); }
static HttpReply httpHead(const std::string& url) { return httpRequest(url, true); }

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

// The release feed, read for the release tags in release order.
//
// The feed is GitHub's own Atom rendering of the releases, and it lists the
// prereleases as well as the releases, newest first - which is what a
// Linux-only release needs, because such a release is a prerelease until its
// Windows archive ships.
//
// It is the FEED and not the releases page on purpose: the page renders the
// asset list lazily, so the page's HTML carries the release notes but not the
// assets. A scan of the page for the archive name therefore reads the notes
// - and the notes are prose, which a future release is free to mention or not
// mention.
// The tags out of a release feed, in the order the feed lists them.
//
// Split out from the request so the parsing can be tested without a network:
// the shapes that matter are the entry id that ends with the tag, the feed's
// own id that ends with "releases" and is not a release at all, and an id
// whose last segment is not a version.
std::vector<std::string> releaseTagsFromFeed(const std::string& body) {
    std::vector<std::string> out;
    // each entry's id ends with the tag:
    //   <id>tag:github.com,2008:Repository/1373058058/v2.7.0</id>
    const std::string kIdOpen = "<id>";
    const std::string kIdClose = "</id>";
    size_t pos = 0;
    while (true) {
        size_t at = body.find(kIdOpen, pos);
        if (at == std::string::npos) break;
        size_t start = at + kIdOpen.size();
        size_t end = body.find(kIdClose, start);
        if (end == std::string::npos) break;
        std::string id = body.substr(start, end - start);
        pos = end;
        // the feed's first id is the feed itself, whose last path segment is
        // "releases" and not a tag
        size_t slash = id.rfind('/');
        if (slash == std::string::npos) continue;
        std::string tag = id.substr(slash + 1);
        int a = 0, b = 0, c = 0;
        if (cleanTag(tag) && parseVersion(tag, a, b, c)) out.push_back(tag);
    }
    return out;
}

static std::vector<std::string> fetchReleaseTags(std::string* why) {
    HttpReply r = httpGet(kReleaseFeedUrl);
    if (r.code != CURLE_OK || r.status != 200 || r.body.empty()) {
        if (why) *why = httpFailureText(r.code, r.status);
        return std::vector<std::string>();
    }
    return releaseTagsFromFeed(r.body);
}

// The download URL of a release's Linux archive. The name follows the
// pattern the release script writes (scripts/make-release-tarball.sh):
// mnpdf-linux-x64-<tag>.tar.gz
static std::string linuxAssetUrl(const std::string& tag) {
    return std::string(kGitHubRoot) + "/" + kRepoPath + "/releases/download/" + tag +
           "/mnpdf-linux-x64-" + tag + ".tar.gz";
}

// Does this release really carry the Linux archive? The download URL answers
// with a redirect to the file when the asset is there and with 404 when it is
// not, so a HEAD on it is the answer - the same URL a reader would click.
//
// The redirects are not followed: the answer that matters is whether the asset
// exists, and the redirect itself is that answer.
// The answer to "does this release carry the Linux archive" is three-valued:
// yes, no, and "the host did not answer". The third is not the same as the
// second - a timeout or a refused connection is not evidence that the archive
// is missing, and reporting it as missing would tell the reader that no
// release carries a Linux build when the network simply hiccuped.
enum class AssetAnswer { Yes, No, Unreachable };

static AssetAnswer probeLinuxAsset(const std::string& tag) {
    HttpReply r = httpHead(linuxAssetUrl(tag));
    if (r.code != CURLE_OK) return AssetAnswer::Unreachable;
    // 200 (the file itself) or a redirect to it both mean the asset is there;
    // 404 means it is not
    if (r.status == 200 || r.status == 301 || r.status == 302 || r.status == 307)
        return AssetAnswer::Yes;
    if (r.status == 404 || r.status == 410) return AssetAnswer::No;
    // 403, 429 and the 5xx answers are the host's state, not the asset's
    return AssetAnswer::Unreachable;
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
// the reader who installed it should still be told about the next one.
UpdateResult checkForUpdate() {
    UpdateResult res;
    std::string why;

    // The feed lists the releases newest first, prereleases included, so the
    // first one that really carries the Linux archive is the answer. The feed
    // only says which releases exist; the asset is confirmed by asking for its
    // download URL, because the feed carries no asset list.
    std::vector<std::string> tags = fetchReleaseTags(&why);
    if (tags.empty()) {
        res.error = "The update check could not reach GitHub - " + why + ".";
        return res;
    }
    bool unreachable = false;
    for (const std::string& tag : tags) {
        AssetAnswer a = probeLinuxAsset(tag);
        if (a == AssetAnswer::Unreachable) {
            // the host did not answer for this one; try the next, and remember
            // that the answer was not a "no"
            unreachable = true;
            continue;
        }
        if (a == AssetAnswer::No) continue;
        res.latestTag = tag;
        break;
    }
    if (res.latestTag.empty()) {
        res.error = unreachable
            ? "The update check could not reach GitHub - the release host did not answer."
            : "No release with a Linux build was found.";
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

// The command's half: it honours the cooldown and stamps the attempt, so
// clicking "Check for updates" twice in a row makes one request, not two. It
// also reports the release GitHub calls current, which is a different release
// from the newest one carrying a Linux archive.
//
// The stamp is NOT written here: this runs on the worker thread, and
// writeAppPref writes a file that every other writer touches from the GTK
// main thread. The Windows build has the same split - the worker posts its
// result and the window procedure stamps the attempt (src/main.cpp:6085) -
// so the stamp happens in showUpdateResult, on the UI thread.
bool updateCheckNow(std::string& latestTag, bool& newer, std::string& error,
                    std::string& currentTag, bool& currentIsNewer) {
    if (updateCheckCooling()) {
        // A click inside the cooldown window is answered from what the last
        // attempt learned, so the reader still gets a true answer and no
        // request is spent. The Windows build does the same
        // (src/main.cpp:3156-3180, reportCachedUpdateResult).
        if (gLastUpdateTag.empty()) {
            // the last attempt could not reach GitHub
            int left = kUpdateCheckIntervalMin - minutesSinceLastCheck();
            if (left < 1) left = 1;
            error = "mnpdf checked GitHub for updates recently and could not reach it.\n"
                    "Please try again in " + std::to_string(left) +
                    (left == 1 ? " minute." : " minutes.");
            return false;
        }
        latestTag = gLastUpdateTag;
        newer = versionIsNewer(latestTag);
        currentTag = latestTag;
        currentIsNewer = newer;
        return true;
    }
    UpdateResult r = checkForUpdate();
    if (!r.ok) {
        error = r.error;
        return false;
    }
    latestTag = r.latestTag;
    newer = r.newer;
    currentTag = r.currentTag;
    currentIsNewer = r.currentIsNewer;
    return true;
}

