// fix.hpp — minimal FIX 4.2 text encode/parse + a binary-wire discipline demo.
//
// The live transport is FIX 4.2 over TCP (matching the original Python engine):
// ASCII tag=value fields separated by SOH (0x01), framed by BodyLength(9) and
// CheckSum(10). We hand-roll it so the hot path builds messages into a fixed
// stack buffer with NO heap allocation (email §"Core C++ Language":
// string_view / span / from_chars; §"Memory": no malloc on hot path).
//
// The BinOrder section at the bottom demonstrates the separate topic of
// "POD / trivially-copyable struct discipline: #pragma pack, static_assert,
// is_trivially_copyable, memcpy not reinterpret_cast" — how you'd frame a
// *binary* protocol (ITCH/OUCH-style). It is exercised by the unit tests.
#pragma once
#include "types.hpp"
#include <charconv>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ts::fix {

inline constexpr char SOH = '\x01';

// Checksum over raw bytes = sum(bytes) mod 256. Takes a std::span — a non-owning
// {ptr,len} view (email: std::span) that works over any contiguous byte range.
[[nodiscard]] inline u8 checksum(std::span<const char> bytes) noexcept {
    u32 sum = 0;
    for (char c : bytes) sum += static_cast<u32>(static_cast<unsigned char>(c));
    return static_cast<u8>(sum & 0xFFu);
}

// ---- builder: assemble a message into a caller buffer, no allocation --------
class Builder {
public:
    void reset() noexcept { body_len_ = 0; }

    void add(int tag, std::string_view val) noexcept {
        body_len_ += write_field(body_ + body_len_, tag, val);
    }
    void add(int tag, i64 val) noexcept {
        char num[24];
        const auto r = std::to_chars(num, num + sizeof(num), val);
        add(tag, std::string_view(num, static_cast<std::size_t>(r.ptr - num)));
    }

    // Compose 8=FIX.4.2 | 9=<bodylen> | <body> | 10=<checksum> into `out`.
    // Returns the total message length.
    [[nodiscard]] u32 finalize(char* out) noexcept {
        char lenbuf[16];
        const auto lr = std::to_chars(lenbuf, lenbuf + sizeof(lenbuf), body_len_);
        std::size_t n = 0;
        n += write_field(out + n, 8, "FIX.4.2");
        n += write_field(out + n, 9,
                         std::string_view(lenbuf, static_cast<std::size_t>(lr.ptr - lenbuf)));
        std::memcpy(out + n, body_, body_len_);  // body is 35=... onward
        n += body_len_;
        const u8 ck = checksum(std::span<const char>(out, n));  // over 8..body
        char ckbuf[3] = {static_cast<char>('0' + ck / 100),
                         static_cast<char>('0' + (ck / 10) % 10),
                         static_cast<char>('0' + ck % 10)};
        n += write_field(out + n, 10, std::string_view(ckbuf, 3));
        return static_cast<u32>(n);
    }

private:
    static std::size_t write_field(char* p, int tag, std::string_view val) noexcept {
        char* const start = p;
        const auto r = std::to_chars(p, p + 10, tag);
        p = r.ptr;
        *p++ = '=';
        std::memcpy(p, val.data(), val.size());
        p += val.size();
        *p++ = SOH;
        return static_cast<std::size_t>(p - start);
    }

    char        body_[480];
    std::size_t body_len_{0};
};

// ---- view: read fields out of a received message ----------------------------
class View {
public:
    explicit View(std::string_view msg) noexcept : msg_(msg) {}

    // Linear scan (messages are tiny). from_chars parses the tag with no
    // allocation and no locale overhead (unlike atoi/stringstream).
    [[nodiscard]] std::string_view get(int tag) const noexcept {
        std::size_t i = 0;
        while (i < msg_.size()) {
            const std::size_t eq = msg_.find('=', i);
            if (eq == std::string_view::npos) break;
            int t = 0;
            std::from_chars(msg_.data() + i, msg_.data() + eq, t);
            std::size_t soh = msg_.find(SOH, eq + 1);
            if (soh == std::string_view::npos) soh = msg_.size();
            if (t == tag) return msg_.substr(eq + 1, soh - eq - 1);
            i = soh + 1;
        }
        return {};
    }
    [[nodiscard]] i64 get_int(int tag, i64 def = 0) const noexcept {
        const std::string_view v = get(tag);
        if (v.empty()) return def;
        i64 x = def;
        std::from_chars(v.data(), v.data() + v.size(), x);
        return x;
    }

private:
    std::string_view msg_;
};

// ---- framer: pull complete messages out of a TCP byte stream ----------------
// recv() gives arbitrary chunks; a message spans/coalesces across reads. We use
// BodyLength(9) to know exactly where each message ends. (A production gateway
// would frame in a fixed ring buffer; std::string here keeps it readable and is
// off the measured T2T path.)
class Framer {
public:
    void append(const char* p, std::size_t n) { buf_.append(p, n); }

    [[nodiscard]] std::optional<std::string> next() {
        const std::size_t s = buf_.find("8=");
        if (s == std::string::npos) return std::nullopt;
        const std::size_t nine = buf_.find("\x01" "9=", s);
        if (nine == std::string::npos) return std::nullopt;
        const std::size_t lenstart = nine + 3;  // past SOH '9' '='
        const std::size_t soh_after_len = buf_.find(SOH, lenstart);
        if (soh_after_len == std::string::npos) return std::nullopt;
        int body_len = 0;
        std::from_chars(buf_.data() + lenstart, buf_.data() + soh_after_len, body_len);
        const std::size_t body_start = soh_after_len + 1;
        const std::size_t msg_end = body_start + static_cast<std::size_t>(body_len) + 7;  // "10=xxx"+SOH
        if (buf_.size() < msg_end) return std::nullopt;
        std::string msg = buf_.substr(s, msg_end - s);
        buf_.erase(0, msg_end);
        return msg;
    }

private:
    std::string buf_;
};

// ---- binary wire discipline demo (NOT the live transport) -------------------
#pragma pack(push, 1)
struct BinOrder {
    u8  msg_type;  // 'O'
    u64 clordid;
    u32 sym_id;
    u8  side;
    i64 px;
    i64 qty;
};
#pragma pack(pop)
static_assert(std::is_trivially_copyable_v<BinOrder>);
// #pragma pack(1) removes padding, so the struct is exactly the field sizes:
static_assert(sizeof(BinOrder) == 1 + 8 + 4 + 1 + 8 + 8, "must be tightly packed");

// Serialise/deserialise with memcpy — NOT reinterpret_cast<BinOrder*>(buf).
// Casting a char* to BinOrder* and dereferencing is undefined behaviour: the
// buffer may be misaligned for u64, and it violates strict aliasing. memcpy is
// well-defined and the optimiser turns it into the same loads/stores anyway.
[[nodiscard]] inline u32 encode(const BinOrder& o, char* out) noexcept {
    std::memcpy(out, &o, sizeof(o));
    return static_cast<u32>(sizeof(o));
}
[[nodiscard]] inline BinOrder decode(const char* in) noexcept {
    BinOrder o;
    std::memcpy(&o, in, sizeof(o));
    return o;
}

}  // namespace ts::fix
