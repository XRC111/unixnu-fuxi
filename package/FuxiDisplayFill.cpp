// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Fuxi display bring-up, step 2: write to the scanout the firmware handed over.
//
// Step 1 (package/FuxiDisplayProbe.cpp) reads the FB_HANDOFF record and reports the mode, which proved the
// record is real: base, width, height and pitch are all there and match the panel. This step uses them. It
// fills the buffer with a pattern and then paints a moving bar, so the panel shows something the kernel
// controls rather than whatever the firmware left behind.
//
// Why not the original display class: binding IOMobileFramebufferLegacy means subclassing an Apple class
// whose layout has to be recovered from a prelinked candidate of the exact OS build, and this port has no
// such candidate. Writing the scanout does not need the class at all: the firmware already programmed the
// panel and the MDP, and it publishes the buffer it scans out. Painting that buffer is what makes the
// screen change, and every pixel of it is ordinary cached device memory.
//
// Read the record, never guess it. The record sits at the end of "Display Reserved", is stamped with the
// magic only after base/width/height/pitch are all in place, and is the same authority step 1 uses.
//
// Off unless fuxi-display-fill is set. Values:
//   1  fill the whole buffer with a checkerboard, then stop
//   2  additionally animate a bar across the panel, which proves the kernel keeps painting
// fuxi-display-base and fuxi-display-size name the reserved region, as they do for step 1.

#include <pexpert/pexpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/c++/OSSharedPtr.h>

extern "C" void fuxi_display_probe(void);

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

// Reported through the boot log and readable from a debugger.
volatile uint32_t gFillSteps;
volatile uint32_t gFillWidth;
volatile uint32_t gFillHeight;
volatile uint32_t gFillStride;
volatile uint32_t gFillBaseLow;
volatile uint32_t gFillPixels;
volatile uint32_t gFillFrames;

struct Region {
  OSSharedPtr<IOMemoryMap> keep;
  uintptr_t va = 0;
};

// A physical range has to be mapped before it can be touched; dereferencing the physical address faults.
// kIOMapInhibitCache keeps the writes going straight to the scanout instead of sitting in a cache line the
// display engine will never read.
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

// The handoff record sits at the end of "Display Reserved". Boot arguments name the region so this does
// not have to walk the firmware memory map: fuxi-display-base and fuxi-display-size, the same arguments
// step 1 takes.
bool readHandoff(uint64_t regionBase, uint64_t regionSize, FrameBufferHandoff &handoff)
{
  if (regionSize < kHandoffOffset + sizeof(handoff)) {
    return false;
  }
  const uint64_t recordAt = regionBase + regionSize - kHandoffOffset;
  const uint64_t pagePhys = recordAt & ~static_cast<uint64_t>(0xfff);
  const uint64_t offset = recordAt - pagePhys;

  Region page;
  if (!mapRegion(pagePhys, 0x1000, page)) {
    return false;
  }
  const auto *bytes = reinterpret_cast<const uint8_t *>(page.va + offset);
  __builtin_memcpy(&handoff, bytes, sizeof(handoff));
  return handoff.magic == kHandoffMagic;
}

inline uint32_t packPixel(uint8_t r, uint8_t g, uint8_t b)
{
  // The firmware handoff carries base, size and pitch but not the pixel format, and the panel is a DSI
  // command-mode panel whose buffer the firmware filled as 32-bit XRGB. Blue in the low byte is the
  // layout the step-1 colour bands came out as, so the same packing is used here.
  return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

// Fill the whole buffer with a coarse checkerboard. This is the cheapest possible proof that the kernel
// owns the scanout: if the panel changes, the buffer really is what the display engine reads.
void fillCheckerboard(volatile uint32_t *pixels, uint32_t width, uint32_t height, uint32_t stridePixels,
                      uint32_t cell)
{
  const uint32_t dark = packPixel(0x10, 0x10, 0x18);
  const uint32_t light = packPixel(0x28, 0x30, 0x48);
  for (uint32_t y = 0; y < height; ++y) {
    volatile uint32_t *row = pixels + static_cast<uint64_t>(y) * stridePixels;
    const bool yOdd = ((y / cell) & 1) != 0;
    for (uint32_t x = 0; x < width; ++x) {
      const bool xOdd = ((x / cell) & 1) != 0;
      row[x] = (xOdd != yOdd) ? light : dark;
    }
  }
  gFillPixels = width * height;
}

// Paint one vertical bar at x, over a freshly drawn board. Called repeatedly by the animation.
void paintBar(volatile uint32_t *pixels, uint32_t width, uint32_t height, uint32_t stridePixels,
              uint32_t x, uint32_t barWidth)
{
  const uint32_t bar = packPixel(0xf0, 0x40, 0x20);
  const uint32_t xEnd = (x + barWidth < width) ? x + barWidth : width;
  for (uint32_t y = 0; y < height; y += 4) {   // every fourth row: the bar is wide enough to see
    volatile uint32_t *row = pixels + static_cast<uint64_t>(y) * stridePixels;
    for (uint32_t column = x; column < xEnd; ++column) {
      row[column] = bar;
    }
  }
}

} // namespace

// Paint the scanout the firmware handed over. Called from the same place as the step-1 probe, so the
// report and the painting describe one boot.
extern "C" void fuxi_display_fill(void)
{
  unsigned mode = 0;
  if (!PE_parse_boot_argn("fuxi-display-fill", &mode, sizeof(mode)) || mode == 0) {
    return;
  }
  gFillSteps = 1;

  uint64_t regionBase = 0;
  uint64_t regionSize = 0;
  if (!PE_parse_boot_argn("fuxi-display-base", &regionBase, sizeof(regionBase)) ||
      !PE_parse_boot_argn("fuxi-display-size", &regionSize, sizeof(regionSize))) {
    IOLog("FuxiFill: fuxi-display-base/fuxi-display-size not given\n");
    return;
  }
  gFillSteps = 2;

  FrameBufferHandoff handoff = {};
  if (!readHandoff(regionBase, regionSize, handoff)) {
    IOLog("FuxiFill: no FB_HANDOFF record at the end of the reserved region\n");
    return;
  }
  if (handoff.width == 0 || handoff.height == 0 || handoff.pitchBytes < handoff.width * 4) {
    IOLog("FuxiFill: implausible mode %ux%u pitch %u\n", handoff.width, handoff.height,
          handoff.pitchBytes);
    return;
  }
  gFillSteps = 3;
  gFillWidth = handoff.width;
  gFillHeight = handoff.height;
  gFillStride = handoff.pitchBytes;
  gFillBaseLow = static_cast<uint32_t>(handoff.base);

  const uint64_t bytes = static_cast<uint64_t>(handoff.pitchBytes) * handoff.height;
  Region scanout;
  if (!mapRegion(handoff.base, bytes, scanout)) {
    IOLog("FuxiFill: cannot map the scanout at 0x%llx (%llu bytes)\n", handoff.base, bytes);
    return;
  }
  gFillSteps = 4;

  auto *pixels = reinterpret_cast<volatile uint32_t *>(scanout.va);
  const uint32_t stridePixels = handoff.pitchBytes / 4;

  fillCheckerboard(pixels, handoff.width, handoff.height, stridePixels, 64);
  gFillFrames = 1;
  gFillSteps = 5;
  IOLog("FuxiFill: checkerboard on %ux%u at 0x%llx stride %u\n", handoff.width, handoff.height,
        handoff.base, stridePixels);

  if (mode >= 2) {
    // A bar that walks across the panel. Each pass redraws the board first, so the previous position is
    // erased and the motion is visible rather than a smeared trail.
    const uint32_t barWidth = handoff.width / 8;
    for (unsigned pass = 0; pass < 3; ++pass) {
      for (uint32_t x = 0; x + barWidth < handoff.width; x += barWidth / 2) {
        paintBar(pixels, handoff.width, handoff.height, stridePixels, x, barWidth);
        ++gFillFrames;
      }
      fillCheckerboard(pixels, handoff.width, handoff.height, stridePixels, 64);
    }
    IOLog("FuxiFill: %u frames painted\n", static_cast<unsigned>(gFillFrames));
  }
  gFillSteps = 6;
}
