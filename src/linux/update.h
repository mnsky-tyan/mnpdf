// mnpdf for Linux - the update check's API.
//
// One header for the check's two halves and the version rules they share:
// main.cpp runs the blocking half on a worker thread, update.cpp implements
// it, and tests/core-test.cpp drives the parsing, the comparison and the
// release lookup directly.
// They live here rather than in doc.h because the check is not part of the
// document model; doc.h keeps only the cooldown's own state, which
// sidecar.cpp is the one to persist.
#pragma once

#include <functional>
#include <string>
#include <vector>

// The version this build reports. It is generated at configure time from the
// Windows kAppVersion constant (CMakeLists.txt), so the two readers can never
// claim different versions - they share one app.txt.
#include "mnpdf-version.h"

// The Windows version rules (src/main.cpp:2334): a tag is MAJOR.MINOR.PATCH
// with an optional leading v, and a tag that does not parse is never an
// update. `versionIsNewer` compares against this build's own version.
bool parseVersion(const std::string& text, int& major, int& minor, int& patch);
bool versionIsNewer(const std::string& candidate);

// The tags out of a release feed, in the order the feed lists them. Split out
// from the request so the parsing can be exercised without a network: the
// shapes that matter are the entry id that ends with the tag, the feed's own
// id that ends with "releases" and is not a release at all, and an id whose
// last segment is not a version.
std::vector<std::string> releaseTagsFromFeed(const std::string& body);

// Does a release carry the Linux archive? The answer is three-valued: yes, no,
// and "the host did not answer". The third is not the second - a timeout or a
// refused connection is not evidence that the asset is missing.
enum class AssetAnswer { Yes, No, Unreachable };

// The newest release that carries a Linux archive, from the feed's tags in
// release order (newest first). An Unreachable answer stops the search and
// sets `unknown`, because the answer is then unknown, not "not there":
// falling through to the next tag would name an OLDER release as the newest,
// telling a reader on that older build it is up to date. `unknown` is false
// when every tag really answered No.
std::string pickLinuxRelease(const std::vector<std::string>& tags,
                             const std::function<AssetAnswer(const std::string&)>& probe,
                             bool* unknown);

// The check's blocking half, run on a worker thread: it honours the cooldown,
// so a click inside the hour spends no request, and `requested` says whether a
// request was really made. An answer taken from the cache is not one, and must
// not stamp the cooldown - a stamp would push the cooldown's end away, so a
// reader who clicks once an hour would never ask again. The stamp itself is
// written by noteUpdateCheck, on the UI thread: it writes the shared app.txt.
bool updateCheckNow(std::string& latestTag, bool& newer, std::string& error,
                    std::string& currentTag, bool& currentIsNewer, bool& requested);
void noteUpdateCheck(const std::string& tag);
bool updateCheckCooling();

// the whole check - every request it makes - fits in this budget
int updateCheckBudgetSeconds();
