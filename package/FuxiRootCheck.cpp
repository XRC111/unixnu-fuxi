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
// Reading the shipped root image offline settled why. The OS window is a GPT disk whose single APFS
// partition holds launchd and no dyld cache at all; the dyld shared cache (dyld_shared_cache_arm64.01
// through .52, .dylddata, .dyldlinkedit, .symbols) lives in the SystemOS window, which is a bare APFS
// container and is not mounted anywhere. So the loader is told "no such file, no dyld cache" because
// both statements are true of the volume that is mounted.
//
// This probe reports the layout the kernel actually sees after root is mounted, so the mount that
// comes next is chosen from observation. Read-only: it looks paths up and never writes.
//
// fuxi-root-check=1 logs the report; fuxi-root-check=2 also panics afterwards, because on a verbose
// boot the report scrolls away before the panel stops and the panic text is what stays visible.

#include <pexpert/pexpert.h>
#include <IOKit/IOLib.h>
#include <sys/vnode.h>
#include <sys/namei.h>
#include <sys/mount.h>
#include <sys/vfs_context.h>

extern "C" {
// rootvnode is the mounted root vnode; bsd_init sets it before this probe runs.
extern vnode_t rootvnode;

// rootdevice is the block device name vfs_mountroot settled on, and the string bsd_init hands to
// IOSecureBSDRoot. Printing it says which of the two windows became the root.
extern char rootdevice[];

// sys/namei.h declares namei and nameidone without C linkage. This file is compiled as C++, so those
// declarations produce the mangled names namei(nameidata*) and nameidone(nameidata*), which nothing
// defines; the linker answers "found _namei in vfs_lookup.o, declaration possibly missing extern C".
// Redeclaring them as C functions collides with the header, so the two entry points are named by asm
// label instead: the compiler emits the plain symbol whatever the header said.
int fuxi_namei(struct nameidata *ndp) __asm("_namei");
void fuxi_nameidone(struct nameidata *ndp) __asm("_nameidone");
}

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
  const int error = fuxi_namei(&nd);
  if (error == 0) {
    vnode_t vp = nd.ni_vp;
    // A directory is worth telling apart from a file: the cache directory has to be a directory.
    const int isdir = vnode_isdir(vp);
    IOLog("FuxiRoot: %-56s exists%s\n", path, isdir ? " (dir)" : "");
    vnode_put(vp);
    fuxi_nameidone(&nd);
    return true;
  }
  IOLog("FuxiRoot: %-56s missing (error %d)\n", path, error);
  return false;
}

// Name the volume the root actually came from. The two windows of the root image are a GPT disk and a
// bare APFS container; which one answered decides where the dyld cache can possibly be, so the answer is
// part of the report rather than an assumption.
void reportMount(void)
{
  struct mount *mp = rootvnode->v_mount;
  if (mp == nullptr) {
    IOLog("FuxiRoot: root vnode has no mount\n");
    return;
  }
  IOLog("FuxiRoot: root from \"%s\" type \"%s\" on \"%s\" flags 0x%x\n",
        mp->mnt_stat.f_mntfromname, mp->mnt_stat.f_fstypename, mp->mnt_stat.f_mntonname,
        static_cast<unsigned>(mp->mnt_flag));
  IOLog("FuxiRoot: rootdevice \"%s\"\n", rootdevice);
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

  reportMount();

  uint32_t found = 0;
  for (const char *path : kProbes) {
    if (exists(path)) {
      ++found;
    }
  }
  gRootCheckFound = found;
  gRootCheckSteps = 3;

  const uint32_t total = static_cast<uint32_t>(sizeof(kProbes) / sizeof(kProbes[0]));
  IOLog("FuxiRoot: %u of %u probe paths present\n", found, total);

  // On a verbose boot the lines above are already gone from the panel by the time it stops, so the
  // summary is repeated as the panic reason, which stays put.
  if (mode >= 2) {
    panic("fuxi-root-check: %u/%u paths present, root from %s", found, total,
          rootvnode->v_mount ? rootvnode->v_mount->mnt_stat.f_mntfromname : "?");
  }
}
