// SPDX-License-Identifier: GPL-3.0-or-later
#include "KnownDevices.h"

namespace pd {

const std::vector<KnownDevice>& knownDevices() {
    using B = KnownDevice::Bus;
    static const std::vector<KnownDevice> list = {
        {B::USB, 0x04bb, 0x0532, "I-O Data GV-USB2", "", "gvusb2-video,gvusb2_video,gvusb2-sound,gvusb2_sound",
         false, "https://github.com/Isaac-Lozano/GV-USB2-Driver",
         "Out-of-tree driver: build gvusb2-video and gvusb2-sound against your kernel (README, section 'GV-USB2 driver'). "
         "Rebuild after every kernel update. If upstream does not build on your kernel, try the jkl1337 fork: "
         "https://github.com/jkl1337/GV-USB2-Driver",
         {0, 1}},
        {B::USB, 0x1b71, 0x3002, "EasyCap (Fushicai USBTV007)", "USBTV007", "usbtv", true, "https://www.kernel.org/doc/html/latest/admin-guide/media/usbtv.html",
         "Mainline usbtv driver. It also provides the ALSA card; old guides saying there is no audio are out of date.", {0, 1}},
        {B::USB, 0x1f71, 0x3301, "EasyCap (Fushicai USBTV007)", "USBTV007", "usbtv", true, "", "Mainline usbtv driver.", {0, 1}},
        {B::USB, 0x1f71, 0x3306, "EasyCap (Fushicai USBTV007)", "USBTV007", "usbtv", true, "", "Mainline usbtv driver.", {0, 1}},
        {B::USB, 0x05e1, 0x0408, "EasyCap (Syntek STK1160)", "STK1160 + SAA7113", "stk1160", true, "https://www.linuxtv.org/wiki/index.php/Stk1160",
         "Mainline stk1160 driver (since 3.7). Audio is silent until the stk1160-mixer capture source is Line; PingDub sets it. "
         "Some two-chip units only have 8-bit mono audio.", {0, 1}},
        {B::USB, 0xeb1a, 0x2860, "Empia EM2860 device", "EM2860", "em28xx", true, "https://www.linuxtv.org/wiki/index.php/Em28xx_devices",
         "Mainline em28xx driver; audio through em28xx-alsa.", {0, 1}},
        {B::USB, 0xeb1a, 0x2861, "EasyCap (Empia EM2861)", "EM2861", "em28xx", true, "https://www.linuxtv.org/wiki/index.php/Easycap",
         "Mainline em28xx driver; audio through em28xx-alsa.", {0, 1}},
        {B::USB, 0xeb1a, 0x2862, "Empia EM2862 device", "EM2862", "em28xx", true, "", "Mainline em28xx driver.", {0, 1}},
        {B::USB, 0xeb1a, 0x2863, "Empia EM2863 device", "EM2863", "em28xx", true, "", "Mainline em28xx driver.", {0, 1}},
        {B::USB, 0x0fd9, 0x0033, "Elgato Video Capture", "EM2860", "em28xx", true, "", "Mainline em28xx driver.", {0, 1}},
        {B::USB, 0x2304, 0x021a, "Dazzle DVC100", "EM2860", "em28xx", true, "", "Mainline em28xx driver.", {0, 1}},
        {B::USB, 0x2304, 0x0207, "Pinnacle Dazzle DVC90/100", "EM2820", "em28xx", true, "", "Mainline em28xx driver.", {0, 1}},
        {B::USB, 0x1c88, 0x0007, "Somagic EasyCap (no firmware yet)", "SMI2021", "", false,
         "https://github.com/9ary/easycap-somagic-linux",
         "No kernel driver. PingDub drives it from userspace (experimental): it needs the firmware from Somagic's Windows driver "
         "(SmiUsbGrabber3C.sys) once, and a udev rule for USB access. Both are offered in Diagnostics.", {0, 1}},
        {B::USB, 0x1c88, 0x003c, "Somagic EasyCap DC60", "SMI2021 + SAA7113", "", false, "",
         "Driven by PingDub's experimental userspace backend.", {0, 1}},
        {B::USB, 0x1c88, 0x003e, "Somagic EasyCap", "SMI2021", "", false, "", "Driven by PingDub's experimental userspace backend.", {0, 1}},
        {B::USB, 0x1c88, 0x003f, "Somagic EasyCAP002", "SMI2021", "", false, "", "Driven by PingDub's experimental userspace backend.", {0, 1}},
        {B::USB, 0x2040, 0xc200, "Hauppauge USB-Live2", "CX231xx", "cx231xx", true, "https://www.linuxtv.org/wiki/index.php/Hauppauge_USB-Live-2",
         "Mainline cx231xx driver; audio through cx231xx-alsa.", {0, 1}},
        {B::PCI, 0x109e, 0x036e, "Brooktree Bt878 card", "Bt878", "bttv", true, "https://www.kernel.org/doc/html/latest/admin-guide/media/bttv.html",
         "Mainline bttv. Many Bt878 cards have no audio capture; run a cable from the card to the sound card's line in and pick that "
         "device. Two independent clocks then, which video-master sync corrects.", {0, 1}},
        {B::PCI, 0x109e, 0x0350, "Brooktree Bt848 card", "Bt848", "bttv", true, "", "Mainline bttv.", {0, 1}},
        {B::PCI, 0x14f1, 0x8800, "Conexant CX2388x card", "CX2388x", "cx88xx,cx8800", true, "", "Mainline cx88.", {0, 1}},
        {B::PCI, 0x1131, 0x7134, "Philips SAA7134 card", "SAA7134", "saa7134", true, "", "Mainline saa7134.", {0, 1}},
        {B::PCI, 0x1131, 0x7133, "Philips SAA7133/5 card", "SAA7133", "saa7134", true, "", "Mainline saa7134.", {0, 1}},
        {B::PCI, 0x1131, 0x7130, "Philips SAA7130 card", "SAA7130", "saa7134", true, "", "Mainline saa7134 (no audio DMA on 7130).", {0, 1}},
        {B::PCI, 0x14f1, 0x8852, "Conexant CX23885 card", "CX23885", "cx23885", true, "", "Mainline cx23885.", {0, 1}},
        {B::PCI, 0x14f1, 0x8880, "Conexant CX23887/8 card", "CX23888", "cx23885", true, "", "Mainline cx23885.", {0, 1}},
        {B::PCI, 0x14f1, 0x5b7a, "Hauppauge HVR-1600 (CX23418)", "CX23418", "cx18", true, "",
         "Mainline cx18. Use the raw YUV node (HM12), never the MPEG node.", {0, 1}},
        {B::PCI, 0x4444, 0x0016, "Hauppauge PVR (CX23416)", "CX23416", "ivtv", true, "",
         "Mainline ivtv. Use the raw YUV node (HM12), never the MPEG node.", {0, 1}},
        {B::PCI, 0x4444, 0x0803, "Hauppauge PVR (CX23415)", "CX23415", "ivtv", true, "", "Mainline ivtv.", {0, 1}},
        {B::PCI, 0x1cd7, 0xffff, "Magewell Pro Capture", "Magewell", "ProCapture", false, "https://www.magewell.com/downloads",
         "Needs Magewell's vendor driver, which exposes V4L2 and ALSA.", {0, 1}},
        {B::PCI, 0xbdbd, 0xffff, "Blackmagic DeckLink / Intensity", "Blackmagic", "blackmagic,blackmagic_io", false,
         "https://www.blackmagicdesign.com/support/family/capture-and-playback",
         "Needs Blackmagic Desktop Video. PingDub uses the DeckLink API directly (experimental), not V4L2.", {0, 1}},
    };
    return list;
}

const KnownDevice* lookupKnownDevice(KnownDevice::Bus bus, uint16_t vendor, uint16_t product) {
    const KnownDevice* vendorMatch = nullptr;
    for (const KnownDevice& d : knownDevices()) {
        if (d.bus != bus || d.vendor != vendor) continue;
        if (d.product == product) return &d;
        if (d.product == 0xffff) vendorMatch = &d;
    }
    return vendorMatch;
}

std::string missingDriverMessage(const KnownDevice& d) {
    std::string m = std::string(d.name) + " is connected but no driver is bound to it. ";
    if (d.drivers && *d.drivers) {
        std::string drv = d.drivers;
        const size_t c = drv.find(',');
        m += "It needs the " + drv.substr(0, c) + " driver";
        m += d.mainline ? ", which is in the mainline kernel: try 'sudo modprobe " + drv.substr(0, c) + "'. "
                        : ", which is not in the mainline kernel. ";
    }
    m += d.notes;
    if (d.link && *d.link) m += std::string(" More: ") + d.link;
    return m;
}

}  // namespace pd
