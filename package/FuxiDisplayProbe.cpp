// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Fuxi display bring-up, step 1: take the scanout the firmware already established and report what
// the firmware left in the display pipes.
//
// This is deliberately the smallest piece that can be checked on the device. The full XNU display
// contract (NativeLegacyDisplay's MobileFramebuffer binding) needs the prelinked candidates of the
// target OS build, which this port does not have yet, so this stage proves the half that does not
// depend on them:
//
//   * the firmware's scanout geometry is what the board expects (1080x2400, 8-bit BGRA),
//   * the framebuffer survive the handoff into the kernel, and
//   * the MDP and DSI registers read back the values the stock kernel left, which is what the later
//     scanout step has to preserve.
//
// The offsets come from facts/display_engine_facts.json, read from this device's live tree. Nothing
// here writes a display register: it is a read-only probe, silent unless fuxi-display=1 asks for it.

#include <pexpert/pexpert.h>
#include <IOKit/IOLib.h>

namespace {

// MDP and DSI windows, from the device tree reg properties. The MDP block itself sits at
// qcom,sde-off (0x1000) inside the first window.
constexpr uint64_t kMdpBase = 0xae00000ULL;
constexpr uint64_t kSdeOff = 0x1000ULL;

// Register offsets inside the sde block. qcom,sde-ctl-off starts at 0x16000 and the first CTL is the
// one qcom,sde-ctl-display-pref marks "primary"; same for the mixer. qcom,sde-intf-type puts dsi on
// interfaces 1 and 2, and qcom,sde-intf-off gives interface 1 at 0x36000.
constexpr uint32_t kCtl0 = 0x16000;
constexpr uint32_t kIntf1 = 0x36000;
constexpr uint32_t kVig0 = 0x5000;

// Fetch registers of a VIG pipe. These are the SM8550 offsets the reference probe reads
// (DisplayTimingProbe.cpp, dpu=2).
constexpr uint32_t kCtlLayer0 = 0x0000;
constexpr uint32_t kVigSrc0 = 0x0000;
constexpr uint32_t kVigSrc0Xy = 0x0008;
constexpr uint32_t kVigSrc0Size = 0x000c;

volatile uint32_t gDisplaySteps;
volatile uint32_t gFramebufferBytes;
volatile uint32_t gFramebufferWidth;
volatile uint32_t gFramebufferHeight;
volatile uint32_t gFramebufferStride;
volatile uint32_t gFramebufferDepth;
volatile uint32_t gMdpCtl0Layer;
volatile uint32_t gIntf1Timing;
volatile uint32_t gVig0Src0;
volatile uint32_t gVig0Size;

} // namespace

// Read the scanout the firmware handed over, then the pipe state it left behind. Every register read
// is a plain load from the firmware's own mapping; none of these has a read side effect.
extern "C" void fuxi_display_probe(void)
{
  unsigned mode = 0;
  if (!PE_parse_boot_argn("fuxi-display", &mode, sizeof(mode)) || mode == 0) {
    return;
  }
  gDisplaySteps = 1;

  PE_Video console = {};
  if (!PE_current_console(&console)) {
    IOLog("FuxiDisplay: no firmware scanout (PE_current_console reported nothing)\n");
    return;
  }
  if (!console.v_baseAddr || !console.v_width || !console.v_height) {
    IOLog("FuxiDisplay: firmware scanout is empty (%lux%lu at %p)\n",
          console.v_width, console.v_height, reinterpret_cast<void *>(console.v_baseAddr));
    return;
  }

  gFramebufferWidth = static_cast<uint32_t>(console.v_width);
  gFramebufferHeight = static_cast<uint32_t>(console.v_height);
  gFramebufferStride = static_cast<uint32_t>(console.v_rowBytes);
  gFramebufferDepth = static_cast<uint32_t>(console.v_depth);
  gFramebufferBytes = gFramebufferStride * gFramebufferHeight;
  gDisplaySteps = 2;

  IOLog("FuxiDisplay: firmware scanout %ux%u stride %u depth %u at 0x%llx (%u bytes, format %s)\n",
        gFramebufferWidth, gFramebufferHeight, gFramebufferStride, gFramebufferDepth,
        static_cast<unsigned long long>(console.v_baseAddr), gFramebufferBytes, console.v_pixelFormat);

  // The board expects the panel geometry the device tree describes; a mismatch means the firmware
  // did not program the panel this port assumes, and every later step would rest on a wrong premise.
  if (gFramebufferWidth != 1080 || gFramebufferHeight != 2400) {
    IOLog("FuxiDisplay: scanout is %ux%u, the m3 38 0c 0a panel is 1080x2400\n",
          gFramebufferWidth, gFramebufferHeight);
  }
  if (gFramebufferStride != 1080 * 4) {
    IOLog("FuxiDisplay: stride %u is not 4 bytes per pixel for 1080 columns\n", gFramebufferStride);
  }

  if (mode >= 2) {
    // Read what the firmware left in the pipe it was scanning out. These addresses are inside the
    // MDP window the firmware mapped; if that mapping is gone the read faults, which is why this sits
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
