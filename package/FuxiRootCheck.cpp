// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Fuxi root bring-up diagnostics: report what the mounted root actually contains.
//
// The first successful UFS root boot ended at:
//
//   initproc failed to start -- exit reason namespace 6 subcode 0x1
//   Library not loaded: /usr/lib/libSystem.B.dylib
//   Reason: tried: '/usr/lib/libSystem.B.dylib' (no such file, no dyld cache)
//
// On iPadOS 17 that library is not a file on the root volume: it lives in the dyld shared cache, and the
// system volume is only half of the story, because /System/Cryptexes/OS is a separate volume that has to
// be mounted before launchd can resolve anything. This probe answers, from inside the kernel, which of
// those paths exist after root is mounted, so the next step is chosen from observation rather than from a
// guess about the layout.
//
// Read-only: it looks paths up and never writes. Silent unless fuxi-root-check is set.

#include <pexpert/pexpert.h>
#include <IOKit/IOLib.h>
#include <sys/vnode.h>
#include <sys/namei.h>
#include <sys/vfs_context.h>

namespace {

// The paths that decide whether launchd can start. Each is checked with namei, the same lookup the loader
// would do, so a miss here is a miss there.
const char *const kProbes[] = {
  "/usr/lib/libSystem.B.dylib",
  "/usr/lib/dyld",
  "/System/Library/Caches/com.apple.dyld",
  "/System/Library/dyld",
  "/System/Cryptexes",
  "/System/Cryptexes/OS",
  "/System/Cryptexes/App",
  "/sbin/launchd",
  "/private/var",
  "/private/preboot",
};

volatile uint32_t gRootCheckSteps;
volatile uint32_t gRootCheckFound;

// Look a path up under the mounted root and report whether it resolved.
bool exists(const char *path)
{
  struct nameidata nd;
  NDINIT(&nd, LOOKUP, OP_LOOKUP, FOLLOW | AUDITVNPATH1, UIO_SYSSPACE, CAST_USER_ADDR_T(path),
         vfs_context_kernel());
  const int error = namei(&nd);
  if (error == 0) {
    vnode_t vp = nd.ni_vp;
    // A directory is worth telling apart from a file: the cache directory has to be a directory.
    const int isdir = vnode_isdir(vp);
    IOLog("FuxiRoot: %-56s exists%s\n", path, isdir ? " (dir)" : "");
    vnode_put(vp);
    nameidone(&nd);
    return true;
  }
  IOLog("FuxiRoot: %-56s missing (error %d)\n", path, error);
  return false;
}

} // namespace

// Report the root layout the kernel actually sees. Called from bsd_init after the root vnode is set and
// before the first exec, so the answer describes the state launchd will meet.
extern "C" void fuxi_root_check(void)
{
  unsigned mode = 0;
  if (!PE_parse_boot_argn("fuxi-root-check", &mode, sizeof(mode)) || mode == 0) {
    return;
  }
  gRootCheckSteps = 1;

  if (!rootvnode) {
    IOLog("FuxiRoot: no rootvnode yet\n");
    return;
  }
  gRootCheckSteps = 2;

  uint32_t found = 0;
  for (const char *path : kProbes) {
    if (exists(path)) {
      ++found;
    }
  }
  gRootCheckFound = found;

  IOLog("FuxiRoot: %u of %u probe paths present\n",
        found, static_cast<uint32_t>(sizeof(kProbes) / sizeof(kProbes[0])));
  gRootCheckSteps = 3;
}
