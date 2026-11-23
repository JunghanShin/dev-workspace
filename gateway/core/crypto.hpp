// 비밀번호·토큰에 쓰는 작은 암호 도구(docs/gateway.md 3장). OpenSSL 없이 직접 만든다(작게, 의존 없이).
//   SHA-256(FIPS 180-4), HMAC-SHA256(RFC 2104), PBKDF2-HMAC-SHA256(RFC 8018), base64(RFC 4648), hex.
// 시스템 호출 없음. 무작위 바이트는 부르는 쪽(src)이 /dev/urandom에서 받아 넘긴다.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace gw {

class Sha256 {
public:
    Sha256() { reset(); }
    void reset();
    void update(const void* data, size_t n);
    void update(const std::string& s) { update(s.data(), s.size()); }
    std::array<uint8_t, 32> finish();  // 끝내고 값을 돌려준다(다시 쓰려면 reset)

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t used_ = 0;
    uint64_t total_ = 0;
};

std::string sha256(const std::string& data);       // 32바이트(이진)
std::string sha256_hex(const std::string& data);   // 64자 소문자 16진
std::string hmac_sha256(const std::string& key, const std::string& data);
// PBKDF2-HMAC-SHA256. dk_len 바이트(이진)
std::string pbkdf2_sha256(const std::string& password, const std::string& salt, uint32_t iterations, size_t dk_len);

std::string to_hex(const std::string& bin);
std::string base64_encode(const std::string& bin);
bool base64_decode(const std::string& text, std::string& out);  // 패딩 포함 표준 알파벳만, 틀리면 false

// 같은 길이면 끝까지 비교(걸린 시간으로 값을 알아내지 못하게). 길이가 다르면 false
bool constant_time_equal(const std::string& a, const std::string& b);

}  // namespace gw
