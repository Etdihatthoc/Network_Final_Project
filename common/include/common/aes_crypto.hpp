#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace quiz {

// AES-256-CBC encryption key (32 bytes) - SHARED with Node.js gateway
// DO NOT use in production - this is for educational purposes only!
extern const std::uint8_t AES_KEY[32];

// AES-256-CBC IV (16 bytes) - SHARED with Node.js gateway
// DO NOT use in production - this is for educational purposes only!
extern const std::uint8_t AES_IV[16];

// Encrypt plaintext using AES-256-CBC
// Returns encrypted data, or empty vector on error (error string filled)
std::vector<std::uint8_t> encrypt_aes_cbc(
    const std::uint8_t* plaintext,
    std::size_t plaintext_len,
    std::string& error
);

// Decrypt ciphertext using AES-256-CBC
// Returns decrypted data, or empty vector on error (error string filled)
std::vector<std::uint8_t> decrypt_aes_cbc(
    const std::uint8_t* ciphertext,
    std::size_t ciphertext_len,
    std::string& error
);

}  // namespace quiz
