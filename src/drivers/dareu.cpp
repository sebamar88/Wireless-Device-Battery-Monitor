// DAREU EM901X receiver (260D:1074) and USB cable (260D:1090), verified on hardware.
// Battery query from the official EM901X AIM-WL driver v1.0 (GetPower / SendCommand); protocol analysis:
// github.com/wushuai0802/myfinder-device-battery/blob/main/engine/upstream-protocols.md
// Unlike the Kysona transport, Feature 08 is sent on FF02:0002 and Input 09 arrives on
// the sibling FF01:0000 collection. Only command 04 is needed; no initialization/settings.
// Reply: [09][04][status=0]...[6=percent][7=charging]. No reply checksum rule is established.
// Sharkoon Skiller SGM50W (260D:1125, issue #1377): Sharkoon's firmware updater for it is a Compx tool
// (DeviceVidPid=260d1124, the ID it flashes over the cable) and the device exposes the same FF02:0002 feature-17 /
// FF01:0000 input-17 pair, so the same query is sent. Not tested on hardware.
//
// DAREU A84 keyboard (260D:0022, SONiX chip). From the official "DAREU A84 THREE MODE MECHANICAL
// KEYBOARD V1.6" driver (dareu.com/pages/keyboard-driver): its launcher (DeviceDriver.exe, config
// `vid="260D" pid="0022"`) opens the device's interface 0 (path containing "mi_00", the keyboard
// collection with the 65-byte feature report) and, on a timer, sends one command only:
//   SetFeature [00][04 F6 00 ...] (65 bytes), then GetFeature report 0, three reads discarded,
//   then up to three reads until byte 15 is non-zero; byte 5 is the level in percent.
// That is the launcher's only HID command, used just for its battery icon. No charging flag is read.
#include "drivers.h"


// DAREU Togran protocol on receiver 260D:1131 and A980 Pro cable 260D:1148 (verified on hardware), read from DAREU's own driver
// feature report 0A (65 bytes) on the consumer-control collection: [0A][slot<<4][len][class][cmd], reply echoes class/cmd
// slot 0 class 00/A0 lists paired mice, slot n class 07/80 gives [7]=status [8]=level [9]=max, status 1 = charging
static bool TogranQuery(HidDev& dev, BYTE slot, BYTE len, BYTE cls, BYTE cmd, std::vector<BYTE>& r) {
    std::vector<BYTE> req(65, 0);
    req[0] = 0x0A;
    req[1] = (BYTE)(slot << 4);
    req[2] = len;
    req[3] = cls;
    req[4] = cmd;
    if (!dev.SetFeature(req)) return false;
    for (int tries = 0; tries < 15; tries++) {
        Sleep(30);
        if (!dev.GetFeature(0x0A, r) || r.size() < 11) return false;
        if ((r[1] & 0x0F) == 2 && r[3] == cls && r[4] == cmd) return true;
    }
    return false;
}

static void AddTogranBattery(PollContext& ctx, HidDev& dev, BYTE slot, USHORT pid, USHORT mousePid) {
    std::vector<BYTE> b;
    if (!TogranQuery(dev, slot, 4, 0x07, 0x80, b)) return;
    BYTE maxLevel = b[9] ? b[9] : 100;
    if (b[8] > maxLevel) return;
    DeviceStatus s;
    s.key = L"dareu:260D:" + Hex4(pid) + (pid == mousePid ? L"" : L":" + Hex4(mousePid));
    s.name = mousePid == 0x1148 ? L"DAREU A980 Pro" : L"DAREU " + Hex4(mousePid);
    s.kind = DeviceKind::Mouse;
    s.via = slot ? L"2.4 GHz receiver" : L"USB cable";
    s.percent = (int)b[8] * 100 / maxLevel;
    s.charging = b[7] == 1;
    ctx.Add(s, 0x260D, pid);
}

// receiver 1131 lists mice by slot, the A980 Pro on its cable (1148) answers the battery query on slot 0.
// while wired the receiver keeps the mouse listed with status 0, so it isn't shown twice
static void PollDareuTogran(PollContext& ctx) {
    for (USHORT pid : { USHORT(0x1131), USHORT(0x1148) }) {
        if (ctx.IsClaimed(0x260D, pid)) continue;
        auto colls = ctx.Find(0x260D, pid, [](const HidColl& c) {
            return c.page() == 0x000C && c.usage() == 1 && c.featLen() == 65;
        });
        if (colls.size() != 1) continue;
        HidDev dev;
        if (!dev.OpenFeatureOnly(*colls[0])) continue;
        if (pid != 0x1131) {
            AddTogranBattery(ctx, dev, 0, pid, pid);
            continue;
        }
        std::vector<BYTE> r;
        if (!TogranQuery(dev, 0, 6, 0x00, 0xA0, r)) continue;
        int count = r[7];
        for (int slot = 1; slot <= count && slot <= 5; slot++) {
            const BYTE* e = &r[8 + (slot - 1) * 3];
            if (e[0]) AddTogranBattery(ctx, dev, (BYTE)slot, pid, (USHORT)((e[1] << 8) | e[2]));
        }
    }
}

namespace {

void PollDareuA84(PollContext& ctx) {
    const USHORT vid = 0x260D, pid = 0x0022;
    if (ctx.IsClaimed(vid, pid)) return;
    auto colls = ctx.Find(vid, pid, [](const HidColl& c) {
        return c.iface == 0 && c.page() == 0x01 && c.usage() == 0x06 && c.featLen() == 65;
    });
    if (colls.size() != 1) return;
    HidDev d;
    if (!d.OpenFeatureOnly(*colls[0])) return;
    std::vector<BYTE> req(65, 0), r;
    req[1] = 0x04;
    req[2] = 0xF6;
    if (!d.SetFeature(req)) return;
    Sleep(10);
    for (int i = 0; i < 3; i++) {  // the official driver discards these reads
        d.GetFeature(0, r);
        Sleep(10);
    }
    for (int i = 0; i < 3; i++) {
        if (i) Sleep(10);
        if (!d.GetFeature(0, r) || r.size() < 16 || r[15] == 0) continue;
        if (r[5] > 100) return;
        DeviceStatus s;
        s.key = L"dareu:" + Hex4(vid) + L":" + Hex4(pid);
        s.name = L"DAREU A84";
        s.kind = DeviceKind::Keyboard;
        s.via = L"HID";
        s.percent = r[5];
        ctx.Add(s, vid, pid);
        return;
    }
}

}  // namespace

void PollDareu(PollContext& ctx) {
    PollDareuTogran(ctx);
    PollDareuA84(ctx);
    const USHORT vid = 0x260D;
    // Prefer a valid cable reading over potentially cached receiver telemetry.
    bool em901xRead = false;
    for (USHORT pid : { USHORT(0x1090), USHORT(0x1074), USHORT(0x1125) }) {
        if (ctx.IsClaimed(vid, pid) || (em901xRead && pid == 0x1074)) continue;
        auto inputs = ctx.Find(vid, pid, [](const HidColl& c) {
            return c.iface == 1 && c.page() == 0xFF01 && c.usage() == 0 && c.inLen() == 17;
        });
        auto features = ctx.Find(vid, pid, [](const HidColl& c) {
            return c.iface == 1 && c.page() == 0xFF02 && c.usage() == 2 && c.featLen() == 17;
        });
        // Do not pair arbitrary collections when multiple devices share a product ID.
        if (inputs.size() != 1 || features.size() != 1) continue;

        HidDev input, feature;
        if (!input.Open(*inputs[0]) || !feature.OpenFeatureOnly(*features[0])) continue;
        std::vector<BYTE> req(17, 0), r;
        req[0] = 0x08;
        req[1] = 0x04;
        req[16] = 0x49;  // 0x55 minus the sum of bytes 0..15
        input.Flush();
        if (!feature.SetFeature(req)) continue;
        ULONGLONG end = GetTickCount64() + 1000;
        for (int reports = 0; reports < 32; reports++) {
            int left = (int)((LONGLONG)end - (LONGLONG)GetTickCount64());
            if (left <= 0 || input.Read(r, left) < 0) break;
            if (r.size() != 17 || r[0] != 0x09 || r[1] != 0x04 || r[2] != 0 || r[6] > 100) continue;
            DeviceStatus s;
            s.key = L"dareu:" + Hex4(vid) + L":" + Hex4(pid);
            s.name = pid == 0x1125 ? L"Sharkoon Skiller SGM50W" : L"DAREU EM901X";
            s.kind = DeviceKind::Mouse;
            s.via = pid == 0x1090 ? L"USB cable" : L"2.4 GHz receiver";
            s.percent = r[6];
            s.charging = r[7] != 0;
            ctx.Add(s, vid, pid);
            em901xRead = pid != 0x1125;
            break;
        }
    }
}
