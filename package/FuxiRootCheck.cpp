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
// The panel keeps only the last screenful and a verbose boot writes plenty after this runs, so the
// answer is packed into the panic line: one bit per probe path plus the volume's own identity. The
// per-path lines still go to the serial log, where their length costs nothing.
//
// fuxi-root-check=1 logs the report; fuxi-root-check=2 also panics afterwards, because the panic text
// is what stays on the panel.

#include <pexpert/pexpert.h>
#include <IOKit/IOLib.h>
#include <sys/vnode.h>
#include <sys/namei.h>
#include <sys/mount.h>
#include <sys/vfs_context.h>

extern "C" {
// rootvnode is the mounted root vnode; bsd_init sets it before this probe runs.
extern vnode_t rootvnode;

// rootdevice is the block device name vfs_mountroot settled on. It is the short "disk1s1"-shaped name,
// not the "X on Y" string, and printing it says which of the two windows became the root.
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
//
// The order is the order of the bits in the packed report: bit (kProbeCount - 1 - index). A path may be
// appended but not reordered without changing what an old report means.
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

constexpr uint32_t kProbeCount = sizeof(kProbes) / sizeof(kProbes[0]);
static_assert(kProbeCount <= 32, "the packed report is one uint32");

volatile uint32_t gRootCheckSteps;
volatile uint32_t gRootCheckFound;
volatile uint32_t gRootCheckBits;

// Look a path up under the mounted root. Returns 0 when it resolved (and fills vpp when asked), or the
// lookup's errno: ENOENT says the entry is absent, while EACCES or EIO would say the volume has it but the
// lookup was refused, which is a different problem.
int lookup(const char *path, vnode_t *vpp)
{
  struct nameidata nd;
  NDINIT(&nd, LOOKUP, OP_LOOKUP, FOLLOW | AUDITVNPATH1, UIO_SYSSPACE, CAST_USER_ADDR_T(path),
         vfs_context_kernel());
  const int error = fuxi_namei(&nd);
  if (error == 0) {
    vnode_t vp = nd.ni_vp;
    if (vpp != nullptr) {
      *vpp = vp;
    } else {
      vnode_put(vp);
    }
    fuxi_nameidone(&nd);
    return 0;
  }
  IOLog("FuxiRoot: %-56s missing (error %d)\n", path, error);
  return error;
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

  // The volume's own identity first. struct mount and struct vnode are opaque to a kext, so the answer
  // comes from vfs_get_statfs64() instead of from members; statfs64 carries the device name, the
  // filesystem name and the mount point, which is exactly the three facts wanted here.
  mount_t mp = vnode_mount(rootvnode);
  struct statfs64 sfs;
  bool haveStat = false;
  if (mp != nullptr) {
    vfs_get_statfs64(mp, &sfs);
    haveStat = true;
    IOLog("FuxiRoot: root on \"%s\" from \"%s\" type \"%s\" blocks %llu free %llu\n",
          sfs.f_mntonname, sfs.f_mntfromname, sfs.f_fstypename,
          static_cast<unsigned long long>(sfs.f_blocks),
          static_cast<unsigned long long>(sfs.f_bfree));
  } else {
    IOLog("FuxiRoot: root vnode has no mount\n");
  }
  IOLog("FuxiRoot: rootdevice \"%s\"\n", rootdevice);

  uint32_t bits = 0;
  uint32_t found = 0;
  uint32_t errors = 0;   // four bits per missing path: the low nibble is the first one that missed
  uint32_t errorCount = 0;
  for (uint32_t i = 0; i < kProbeCount; ++i) {
    const int error = lookup(kProbes[i], nullptr);
    if (error == 0) {
      ++found;
      bits |= 1u << (kProbeCount - 1 - i);
    } else {
      // ENOENT (2) says the entry is absent; anything else says the volume has it but refused the lookup,
      // which is a different problem with a different fix. Four bits per error is enough for the errnos
      // that can appear here, and six of them fit in the panic line.
      errors |= static_cast<uint32_t>(error & 0xf) << (4 * errorCount);
      if (errorCount < 6) {
        ++errorCount;
      }
    }
  }
  gRootCheckFound = found;
  gRootCheckBits = bits;
  gRootCheckSteps = 3;

  IOLog("FuxiRoot: %u of %u probe paths present, packed 0x%x, errors 0x%x\n", found, kProbeCount, bits,
        errors);

  // The panel keeps only the last screenful, so the answer goes into the panic line: the count, the bit
  // vector (bit 9 is kProbes[0]) and the errno nibbles for the paths that missed. Everything after this is
  // launchd's own output, which is where the lines above would otherwise be lost.
  if (mode >= 2) {
    if (haveStat) {
      panic("fuxi-root-check: %u/%u bits 0x%x err 0x%x, root %s on %s", found, kProbeCount, bits,
            errors, sfs.f_fstypename, sfs.f_mntfromname);
    } else {
      panic("fuxi-root-check: %u/%u bits 0x%x err 0x%x, root device %s", found, kProbeCount, bits,
            errors, rootdevice);
    }
  }
}
