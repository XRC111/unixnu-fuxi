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

// Describe the volume the root came from. The two windows of the root image are a GPT disk and a bare
// APFS container; which one answered decides where the dyld cache can possibly be, so the answer is part
// of the report rather than an assumption. statfs64 carries the device name, the filesystem name and the
// mount point, which is exactly the three facts wanted here.
void reportMount(void)
{
  // struct mount and struct vnode are opaque in the headers a kext may include, so the volume is asked
  // about through vnode_mount() and vfs_get_statfs64() instead of through members.
  mount_t mp = vnode_mount(rootvnode);
  if (mp == nullptr) {
    IOLog("FuxiRoot: root vnode has no mount\n");
    return;
  }
  struct statfs64 sfs;
  vfs_get_statfs64(mp, &sfs);
  IOLog("FuxiRoot: root on \"%s\" from \"%s\" type \"%s\" blocks %llu free %llu\n",
        sfs.f_mntonname, sfs.f_mntfromname, sfs.f_fstypename,
        static_cast<unsigned long long>(sfs.f_blocks),
        static_cast<unsigned long long>(sfs.f_bfree));
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
    // Silence here was how the first placement of this call was wasted: it sat before vfs_mountroot(),
    // printed one line into a verbose log, and the panel showed nothing about the root at all. Reporting
    // it as a panic means a wrong call site is visible instead of looking like a probe that never ran.
    if (mode >= 2) {
      panic("fuxi-root-check: no rootvnode at the call site (called before vfs_mountroot?)");
    }
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
    panic("fuxi-root-check: %u/%u paths present, root device %s", found, total, rootdevice);
  }
}
