// Akko V9 Dash Pro wireless mouse on its 2.4 GHz Telink receiver (USB 38EE:000C, "Akko 2.4G Wireless Mouse").
// Uses the same 64-byte vendor channel on usage page FF1C, usage 0092 as Evision / CHERRY / ATK devices
// (protocol documented in cherry.cpp, originally from B1Chat/BatteryWatch, MIT; verified on live 38EE:000C):
//   frame:  [0] report ID 4, [1..2] checksum (LE) = sum(bytes[3..end]) & 0xFFFF, [3] type, [4..] payload
//   query:  type 0x1A, payload 03 00 00 55 (read-only status request)
//   reply:  [0] 4, [3] 0x1A, [8] battery percent (0..100), [9] non-zero while charging
#include "drivers.h"

namespace {

constexpr USHORT kVid = 0x38EE;
constexpr USHORT kPid = 0x000C;
constexpr USHORT kPage = 0xFF1C;
constexpr USHORT kUsage = 0x0092;
constexpr size_t kLen = 64;
constexpr BYTE kReportId = 0x04;
constexpr BYTE kStatusType = 0x1A;

std::vector<BYTE> BuildQuery() {
    std::vector<BYTE> f(kLen, 0);
    f[0] = kReportId;
    f[3] = kStatusType;
    f[4] = 0x03;
    f[7] = 0x55;
    unsigned sum = 0;
    for (size_t i = 3; i < f.size(); ++i) sum += f[i];
    f[1] = (BYTE)(sum & 0xFF);
    f[2] = (BYTE)((sum >> 8) & 0xFF);
    return f;
}

bool IsStatusReply(const std::vector<BYTE>& r) {
    return r.size() >= 10 && r[0] == kReportId && r[3] == kStatusType && r[8] <= 100;
}

bool Query(const HidColl& c, std::vector<BYTE>& reply) {
    HidDev d;
    if (!d.Open(c)) return false;
    d.Flush();
    const std::vector<BYTE> query = BuildQuery();
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!d.Write(query, 300)) return false;
        const ULONGLONG end = GetTickCount64() + 400;
        for (;;) {
            const LONGLONG left = (LONGLONG)end - (LONGLONG)GetTickCount64();
            if (left <= 0) break;
            std::vector<BYTE> r;
            if (d.Read(r, (int)left) < 0) break;
            if (IsStatusReply(r)) {
                reply = std::move(r);
                return true;
            }
        }
    }
    return false;
}

}  // namespace

void PollAkko(PollContext& ctx) {
    if (ctx.IsClaimed(kVid, kPid)) return;
    for (const HidColl* c : ctx.Find(kVid, kPid, [](const HidColl& c) {
             return c.page() == kPage && c.usage() == kUsage && c.inLen() == kLen && c.outLen() == kLen;
         })) {
        std::vector<BYTE> reply;
        if (!Query(*c, reply)) continue;

        DeviceStatus s;
        s.key = L"akko:38ee:000c";
        s.name = L"Akko V9 Dash Pro";
        s.kind = DeviceKind::Mouse;
        s.via = L"2.4 GHz receiver";
        s.percent = reply[8];
        s.charging = reply[9] != 0;
        s.graceSec = 1800;
        ctx.Add(s, kVid, kPid);
        return;
    }
}
