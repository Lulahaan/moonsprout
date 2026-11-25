// M5.3b transport: parse_impair (port/net/transport.hpp), the --net-impair / MPPM_NET_IMPAIR grammar.
#include "../transport.hpp"

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>

namespace mppm::net {

namespace {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

// "<number><unit>": the number may have a fraction; returns false on anything else.
bool split_number(const std::string& v, double& num, std::string& unit) {
    size_t i = 0;
    bool digits = false, dot = false;
    while (i < v.size() && (std::isdigit(static_cast<unsigned char>(v[i])) || (v[i] == '.' && !dot))) {
        if (v[i] == '.') dot = true; else digits = true;
        i++;
    }
    if (!digits) return false;
    num = std::strtod(v.substr(0, i).c_str(), nullptr);
    unit = v.substr(i);
    return std::isfinite(num);
}

bool parse_time_us(const std::string& v, uint32_t& out) {
    double n;
    std::string u;
    if (!split_number(v, n, u)) return false;
    double us;
    if (u == "us") us = n;
    else if (u == "ms") us = n * 1e3;
    else if (u == "s") us = n * 1e6;
    else return false;
    if (us > 10e6) return false; // 10 s: anything larger is a typo
    out = static_cast<uint32_t>(std::llround(us));
    return true;
}

bool parse_rate_ppm(const std::string& v, uint32_t& out) {
    double n;
    std::string u;
    if (!split_number(v, n, u)) return false;
    double ppm;
    if (u == "%") ppm = n * 1e4;
    else if (u == "ppm") ppm = n;
    else return false;
    if (ppm > 1e6) return false;
    out = static_cast<uint32_t>(std::llround(ppm));
    return true;
}

} // namespace

bool parse_impair(const std::string& text, Impair& out, std::string& err) {
    Impair r;
    std::string t = trim(text);
    if (t.empty() || t == "off") {
        out = r;
        return true;
    }
    bool seen[5] = {};
    size_t pos = 0;
    while (pos <= t.size()) {
        size_t comma = t.find(',', pos);
        std::string item = trim(t.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
        pos = comma == std::string::npos ? t.size() + 1 : comma + 1;
        size_t eq = item.find('=');
        if (item.empty() || eq == std::string::npos) {
            err = "impairment item '" + item + "' is not key=value";
            return false;
        }
        std::string key = trim(item.substr(0, eq)), val = trim(item.substr(eq + 1));
        int k;
        bool ok;
        if (key == "delay") { k = 0; ok = parse_time_us(val, r.delayUs); }
        else if (key == "jitter") { k = 1; ok = parse_time_us(val, r.jitterUs); }
        else if (key == "loss") { k = 2; ok = parse_rate_ppm(val, r.lossPpm); }
        else if (key == "dup") { k = 3; ok = parse_rate_ppm(val, r.dupPpm); }
        else if (key == "seed") {
            k = 4;
            char* end = nullptr;
            unsigned long long s = val.empty() ? 0 : std::strtoull(val.c_str(), &end, 10);
            ok = !val.empty() && std::isdigit(static_cast<unsigned char>(val[0])) && end && *end == 0 && s <= 0xFFFFFFFFull;
            r.seed = static_cast<uint32_t>(s);
        } else {
            err = "unknown impairment key '" + key + "' (delay, jitter, loss, dup, seed)";
            return false;
        }
        if (!ok) {
            err = "bad impairment value '" + item + "' (times in us, ms or s; rates in % or ppm)";
            return false;
        }
        if (seen[k]) {
            err = "impairment key '" + key + "' given twice";
            return false;
        }
        seen[k] = true;
    }
    if (r.seed == 0) r.seed = 1;
    out = r;
    return true;
}

uint64_t Transport::now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace mppm::net
