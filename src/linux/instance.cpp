// The primary-instance guard.
//
// The Windows build claims a named mutex at startup
// (CreateMutexW(L"Global\\mnpdf.instance"), src/main.cpp:6138) and a window
// that does not own it writes nothing: it leaves app.txt and last.txt alone
// (src/main.cpp:3276, 3344). Without that, a second reader on another
// document overwrites the first one's preferences and the document a plain
// launch would reopen.
//
// The Linux build needs the same rule for the same reason: app.txt is shared
// between the two readers, and a second Linux window must not rewrite it.
//
// The mechanism is an advisory lock on a lock file in the config directory:
// flock() is released by the kernel when the process dies, so a crashed
// reader does not leave the role locked, and the lock is per-machine rather
// than per-user, which matches the "Global\\" prefix the Windows build uses.

#include "doc.h"
#include "ui.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

// true when this process holds the shared-preference role
bool gPrimaryInstance = true;

static int gLockFd = -1;

bool claimPrimaryInstance() {
    std::string dir = appDir();
    std::string path = dir + "/instance.lock";
    int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        // A config directory we cannot write to is not a reason to refuse to
        // read: the window works, it just cannot persist preferences. The
        // Windows build behaves the same way when the mutex cannot be created
        // (src/main.cpp:6138: "A failure to create at all leaves the box
        // primary").
        return true;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        // another reader holds it: this window is the secondary one
        close(fd);
        return false;
    }
    gLockFd = fd;   // held for the process lifetime
    return true;
}

void releasePrimaryInstance() {
    if (gLockFd >= 0) {
        flock(gLockFd, LOCK_UN);
        close(gLockFd);
        gLockFd = -1;
    }
}
