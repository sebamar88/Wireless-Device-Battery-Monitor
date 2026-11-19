// SONiX keyboards on their 2.4 GHz dongle: AJAZZ AK820 MAX ("AJAZZ AK820 MAX 2.4G", 05AC:024F, tested),
// AULA F108 Pro (same 05AC:024F dongle), AJAZZ AK980 V2 (0C45:FEFE) and AJAZZ AK820 Pro (0C45:FDFD,
// some units ship a 0C45:FEFE "2.4G Dongle"). The 05AC dongle reuses Apple's vendor id, so the dongles
// are also matched by their vendor collection: interface 3, usage page FF60 / usage 61, 33-byte input
// and output reports (report id 0 + 32 bytes).
//   request [20 01 00 ... sum]          sum = byte sum of the 31 bytes before it
//   reply   [20 01 00 level ...]        level in percent
// A reply is recognised by its first three bytes (the echoed command), as the AK820 Pro tools do; its
// checksum is not checked. The keyboard only reports its level while it is switched to 2.4G mode.
// The protocol is read-only.
// MCHOSE K87 receivers on the same 05AC:024F id expose a different layout (the BY collection on
// interface 1, no FF60 on interface 3); aula.cpp runs first and reads those.
// From github.com/donglover69/sonix-keyboard-studio (README, "2.4G dongles"); the AK820 Pro receiver
// 0C45:FDFD and the echo match from github.com/deddroid/ak820-rs (src/device.rs, src/protocol.rs) and
// github.com/usharik/ak820-studio (Sources/AK820Kit/Keyboard.swift).
#include "drivers.h"

namespace {

const std::pair<USHORT, USHORT> kDongles[] = { { 0x05AC, 0x024F }, { 0x0C45, 0xFEFE }, { 0x0C45, 0xFDFD } };

const size_t kLen = 32;  // payload bytes after the report id; the last one is the checksum

BYTE Sum(const std::vector<BYTE>& b, size_t from, size_t n) {
    unsigned s = 0;
    for (size_t i = from; i < from + n; i++) s += b[i];
    return (BYTE)(s & 0xFF);
}

// Returns the level byte, or -1 when the dongle did not answer.
int QueryLevel(HidDev& d) {
    std::vector<BYTE> req(1 + kLen, 0);  // [0] = report id 0
    req[1] = 0x20;
    req[2] = 0x01;
    req[kLen] = Sum(req, 1, kLen - 1);
    d.Flush();
    if (!d.Write(req)) return -1;
    ULONGLONG end = GetTickCount64() + 500;
    for (;;) {
        int left = (int)((LONGLONG)end - (LONGLONG)GetTickCount64());
        if (left <= 0) return -1;
        std::vector<BYTE> r;
        int n = d.Read(r, left);  // report id 0 is stripped: r[0] is the first payload byte
        if (n < 0) return -1;
        if (n < 4 || r[0] != 0x20 || r[1] != 0x01 || r[2] != 0x00) continue;
        return r[3];
    }
}

std::wstring KeyboardName(const HidColl& c) {
    std::wstring name = HidProduct(c);
    const std::wstring suffix = L" 2.4G";
    if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
        name.resize(name.size() - suffix.size());
    return name.empty() ? L"Wireless keyboard" : name;
}

}  // namespace

void PollSonix(PollContext& ctx) {
    for (auto [vid, pid] : kDongles) {
        if (ctx.IsClaimed(vid, pid)) continue;
        for (const HidColl* c : ctx.Find(vid, pid, [](const HidColl& c) {
                 return c.iface == 3 && c.page() == 0xFF60 && c.usage() == 0x0061 && c.outLen() == 1 + kLen &&
                        c.inLen() == 1 + kLen;
             })) {
            HidDev d;
            if (!d.Open(*c)) continue;
            // The first request can go unanswered while the 2.4 GHz link wakes up; retry once.
            int level = QueryLevel(d);
            if (level < 0) level = QueryLevel(d);
            if (level < 0) continue;
            DeviceStatus s;
            s.key = L"sonix:" + Hex4(vid) + L":" + Hex4(pid);
            s.name = KeyboardName(*c);
            s.kind = DeviceKind::Keyboard;
            s.via = L"2.4 GHz receiver";
            s.connected = level >= 1 && level <= 100;  // 0: keyboard off, out of range or in another mode
            if (s.connected) s.percent = level;
            s.graceSec = 1800;  // keep the last value if a sleeping keyboard stops answering
            ctx.Add(s, vid, pid);
            break;
        }
    }
}
