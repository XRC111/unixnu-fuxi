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
// This stage is read-only. The full XNU display contract (NativeLegacyDisplay binding to the original
// MobileFramebuffer) needs the prelinked candidates of the target OS build, which this port does not
// have yet, so the probe establishes the facts that step will rest on: where the frame buffer is, how
// big it is, and what the panel geometry really is.

#include <pexpert/pexpert.h>
#include <IOKit/IOLib.h>

namespace {

// From uefi-fuxi Silicon/Silicium/SiliciumPkg/Include/Library/FrameBufferHandoff.h.
constexpr uint32_t kHandoffMagic = 0x4F474246;   // "FBGO"
constexpr uint32_t kHandoffOffset = 0x40;

// The record as the firmware writes it. Packed and little-endian, which is how AArch64 lays it out.
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
constexpr uint64_t kSdeOff = 0x1000ULL;
constexpr uint32_t kCtl0 = 0x16000;
constexpr uint32_t kIntf1 = 0x36000;   // qcom,sde-intf-off for the DSI interface
constexpr uint32_t kVig0 = 0x5000;

// Fetch registers of a VIG pipe, as the reference probe reads them (DisplayTimingProbe.cpp, dpu=2).
constexpr uint32_t kCtlLayer0 = 0x0000;
constexpr uint32_t kVigSrc0 = 0x0000;
constexpr uint32_t kVigSrc0Xy = 0x0008;
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
  const volatile FrameBufferHandoff *record =
      reinterpret_cast<const volatile FrameBufferHandoff *>(base + size - kHandoffOffset);
  if (record->magic != kHandoffMagic) {
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
// plain load from the firmware's own mapping; none of these has a read side effect.
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
  } else {
    IOLog("FuxiDisplay: no FB_HANDOFF record (pass fuxi-display-base and fuxi-display-size)\n");
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
    // What the firmware left in the pipe it was scanning out. These addresses are inside the MDP
    // window the firmware mapped; if that mapping is gone the read faults, which is why this sits
    // behind mode 2 and is not part of the default bring-up.
    volatile uint32_t *sde = reinterpret_cast<volatile uint32_t *>(kMdpBase + kSdeOff);
    gMdpCtl0Layer = sde[(kCtl0 + kCtlLayer0) / 4];
    gIntf1Timing = sde[kIntf1 / 4];
    gVig0Src0 = sde[(kVig0 + kVigSrc0) / 4];
    gVig0Size = sde[(kVig0 + kVigSrc0Size) / 4];
    IOLog("FuxiDisplay: ctl0 layer0 %08x intf1+0 %08x vig0 src0 %08x size %08x\n",
          gMdpCtl0Layer, gIntf1Timing, gVig0Src0, gVig0Size);
    gDisplaySteps = 3;
  }

  IOLog("FuxiDisplay: probe done, steps %u\n", gDisplaySteps);
}
