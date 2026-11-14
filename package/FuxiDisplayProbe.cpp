// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Fuxi display bring-up, step 1: read the scanout the firmware handed over.
//
// The firmware publishes the frame buffer it is really scanning out in a small record at the end of the
// "Display Reserved" memory region (uefi-fuxi, FrameBufferHandoff.h):
//
//   #define FB_HANDOFF_MAGIC  0x4F474246   /* "FBGO" */
//   #define FB_HANDOFF_OFFSET 0x40
//   struct { u32 Magic, Width, Height, PitchBytes; u64 Base; }   /* 24 bytes */
//
//   record = Region.Address + Region.Length - FB_HANDOFF_OFFSET
//
// That record is the authority, not PE_current_console: the console descriptor describes whichever
// display the console code chose, while this record is written by the driver that programmed the panel
// and is stamped with a magic only after base, width, height and pitch are all in place.
//
// Both regions are mapped through IODeviceMemory before they are read. A physical address is not
// reachable from the kernel by dereferencing it: the first version of this probe did that and the boot
// died with a level-1 translation fault, far = 0xbaafffc0, esr = 0x96000005. The touch probe maps its
// controller the same way (package/FuxiTouchProbe.cpp, mapRegion).
//
// This stage is read-only. The full XNU display contract (NativeLegacyDisplay binding to the original
// MobileFramebuffer) needs the prelinked candidates of the target OS build, which this port does not
// have yet, so the probe establishes the facts that step will rest on.

#include <pexpert/pexpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/c++/OSSharedPtr.h>

namespace {

// From uefi-fuxi Silicon/Silicium/SiliciumPkg/Include/Library/FrameBufferHandoff.h.
constexpr uint32_t kHandoffMagic = 0x4F474246;   // "FBGO"
constexpr uint32_t kHandoffOffset = 0x40;

// The record as the firmware writes it, little-endian like the rest of the AArch64 ABI.
struct FrameBufferHandoff {
  uint32_t magic;
  uint32_t width;
  uint32_t height;
  uint32_t pitchBytes;
  uint64_t base;
} __attribute__((packed));

// The board expects this panel; the device tree and the running Android system both say so.
constexpr uint32_t kPanelWidth = 1080;
constexpr uint32_t kPanelHeight = 2400;

// MDP and DSI windows, from the device tree reg properties (facts/display_engine_facts.json). The sde
// block sits at qcom,sde-off inside the first window.
constexpr uint64_t kMdpBase = 0xae00000ULL;
constexpr uint64_t kMdpSize = 0x84000ULL;
constexpr uint64_t kSdeOff = 0x1000ULL;
constexpr uint32_t kCtl0 = 0x16000;
constexpr uint32_t kIntf1 = 0x36000;   // qcom,sde-intf-off for the DSI interface
constexpr uint32_t kVig0 = 0x5000;

// Fetch registers of a VIG pipe, as the reference probe reads them (DisplayTimingProbe.cpp, dpu=2).
constexpr uint32_t kCtlLayer0 = 0x0000;
constexpr uint32_t kVigSrc0 = 0x0000;
constexpr uint32_t kVigSrc0Size = 0x000c;

// Reported through the boot log; also readable from a debugger, which is why they are file-scope.
volatile uint32_t gDisplaySteps;
volatile uint32_t gFramebufferBaseLow;
volatile uint32_t gFramebufferWidth;
volatile uint32_t gFramebufferHeight;
volatile uint32_t gFramebufferStride;
volatile uint32_t gFramebufferBytes;
volatile uint32_t gHandoffFound;
volatile uint32_t gMdpCtl0Layer;
volatile uint32_t gIntf1Timing;
volatile uint32_t gVig0Src0;
volatile uint32_t gVig0Size;

struct Region {
  OSSharedPtr<IOMemoryMap> keep;
  uintptr_t va = 0;
};

// A physical range has to be mapped before it can be read; dereferencing the physical address faults.
bool mapRegion(uint64_t phys, uint64_t length, Region &region)
{
  IODeviceMemory *memory = IODeviceMemory::withRange(phys, length);
  if (!memory) {
    return false;
  }
  region.keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
  memory->release();
  region.va = region.keep ? static_cast<uintptr_t>(region.keep->getVirtualAddress()) : 0;
  return region.va != 0;
}

// The handoff record sits at the end of "Display Reserved", whose address the firmware memory map
// carries. Boot arguments name it so the probe does not have to walk the map: fuxi-display-base and
// fuxi-display-size give the region, the same way the touch probe takes its controller window.
bool readHandoff(FrameBufferHandoff *handoff)
{
  unsigned long long base = 0;
  unsigned long long size = 0;
  if (!PE_parse_boot_argn("fuxi-display-base", &base, sizeof(base)) ||
      !PE_parse_boot_argn("fuxi-display-size", &size, sizeof(size))) {
    return false;
  }
  if (base == 0 || size < kHandoffOffset) {
    return false;
  }

  // Only the last page is needed: the record is the final 0x40 bytes of the region.
  const uint64_t recordPhys = base + size - kHandoffOffset;
  const uint64_t pagePhys = recordPhys & ~0xfffULL;
  const uint64_t offsetInPage = recordPhys - pagePhys;

  Region region;
  if (!mapRegion(pagePhys, 0x1000, region)) {
    IOLog("FuxiDisplay: cannot map the handoff page at 0x%llx\n",
          static_cast<unsigned long long>(pagePhys));
    return false;
  }

  const volatile FrameBufferHandoff *record =
      reinterpret_cast<const volatile FrameBufferHandoff *>(region.va + offsetInPage);
  if (record->magic != kHandoffMagic) {
    IOLog("FuxiDisplay: handoff page mapped, magic is %08x not FBGO\n", record->magic);
    return false;
  }
  handoff->magic = record->magic;
  handoff->width = record->width;
  handoff->height = record->height;
  handoff->pitchBytes = record->pitchBytes;
  handoff->base = record->base;
  return true;
}

} // namespace

// Read what the firmware left scanning out, then the pipe state behind it. Every register read is a
// plain load from a mapping this function creates; none of these has a read side effect.
extern "C" void fuxi_display_probe(void)
{
  unsigned mode = 0;
  if (!PE_parse_boot_argn("fuxi-display", &mode, sizeof(mode)) || mode == 0) {
    return;
  }
  gDisplaySteps = 1;

  FrameBufferHandoff handoff = {};
  gHandoffFound = readHandoff(&handoff) ? 1 : 0;
  if (gHandoffFound) {
    gFramebufferBaseLow = static_cast<uint32_t>(handoff.base);
    gFramebufferWidth = handoff.width;
    gFramebufferHeight = handoff.height;
    gFramebufferStride = handoff.pitchBytes;
    gFramebufferBytes = handoff.pitchBytes * handoff.height;
    gDisplaySteps = 2;
    IOLog("FuxiDisplay: handoff %ux%u pitch %u base 0x%llx (%u bytes)\n",
          handoff.width, handoff.height, handoff.pitchBytes,
          static_cast<unsigned long long>(handoff.base), gFramebufferBytes);
    if (handoff.width != kPanelWidth || handoff.height != kPanelHeight) {
      IOLog("FuxiDisplay: handoff geometry is %ux%u, the m3 38 0c 0a panel is %ux%u\n",
            handoff.width, handoff.height, kPanelWidth, kPanelHeight);
    }
    if (handoff.pitchBytes != kPanelWidth * 4) {
      IOLog("FuxiDisplay: pitch %u is not 4 bytes per pixel for %u columns\n",
            handoff.pitchBytes, kPanelWidth);
    }
  }

  // The console descriptor is a second opinion: when the firmware console and the real scanout agree,
  // both numbers match, and a disagreement is worth knowing before the driver trusts either.
  PE_Video console = {};
  if (PE_current_console(&console) && console.v_baseAddr && console.v_width && console.v_height) {
    IOLog("FuxiDisplay: console %lux%lu rowBytes %lu depth %lu at 0x%llx\n",
          console.v_width, console.v_height, console.v_rowBytes, console.v_depth,
          static_cast<unsigned long long>(console.v_baseAddr));
    if (gHandoffFound &&
        (static_cast<uint32_t>(console.v_width) != gFramebufferWidth ||
         static_cast<uint32_t>(console.v_height) != gFramebufferHeight)) {
      IOLog("FuxiDisplay: console and handoff disagree on geometry\n");
    }
  } else {
    IOLog("FuxiDisplay: no console descriptor\n");
  }

  if (mode >= 2) {
    // What the firmware left in the pipe it was scanning out. The MDP window is mapped the same way as
    // the handoff page; reading it through the physical address would fault.
    Region mdp;
    if (!mapRegion(kMdpBase, kMdpSize, mdp)) {
      IOLog("FuxiDisplay: cannot map the MDP window at 0x%llx\n",
            static_cast<unsigned long long>(kMdpBase));
    } else {
      const uintptr_t sde = mdp.va + kSdeOff;
      gMdpCtl0Layer = *reinterpret_cast<volatile uint32_t *>(sde + kCtl0 + kCtlLayer0);
      gIntf1Timing = *reinterpret_cast<volatile uint32_t *>(sde + kIntf1);
      gVig0Src0 = *reinterpret_cast<volatile uint32_t *>(sde + kVig0 + kVigSrc0);
      gVig0Size = *reinterpret_cast<volatile uint32_t *>(sde + kVig0 + kVigSrc0Size);
      IOLog("FuxiDisplay: ctl0 layer0 %08x intf1+0 %08x vig0 src0 %08x size %08x\n",
            gMdpCtl0Layer, gIntf1Timing, gVig0Src0, gVig0Size);
      gDisplaySteps = 3;
    }
  }

  IOLog("FuxiDisplay: probe done, steps %u\n", gDisplaySteps);
}
