#include "common/aes_crypto.hpp"

#include <openssl/evp.h>
#include <openssl/err.h>
#include <cstring>

namespace quiz {

// AES-256-CBC encryption key (32 bytes) - MUST match Node.js gateway exactly
// These are the same bytes from Lesson 12/common.c
const std::uint8_t AES_KEY[32] = {
    0x4a,0x2f,0x5b,0x6c,0x9d,0x11,0x23,0x34,
    0x45,0x56,0x67,0x78,0x89,0x9a,0xab,0xbc,
    0xcd,0xde,0xef,0xf1,0x12,0x24,0x35,0x46,
    0x57,0x68,0x79,0x8a,0x9b,0xac,0xbd,0xce
};

// AES-256-CBC IV (16 bytes) - MUST match Node.js gateway exactly
const std::uint8_t AES_IV[16] = {
    0x10,0x22,0x33,0x44,0x55,0x66,0x77,0x88,
    0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff,0x01
};

std::vector<std::uint8_t> encrypt_aes_cbc(
    const std::uint8_t* plaintext,
    std::size_t plaintext_len,
    std::string& error
) {
    // Create EVP cipher context
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        error = "Failed to create EVP cipher context";
        return {};
    }

    // Calculate maximum output size: plaintext + block_size for padding
    // AES block size is 16 bytes
    int block_size = EVP_CIPHER_block_size(EVP_aes_256_cbc());
    std::size_t max_output_len = plaintext_len + block_size;
    std::vector<std::uint8_t> ciphertext(max_output_len);

    int len1 = 0, len2 = 0;

    // Initialize encryption with AES-256-CBC
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, AES_KEY, AES_IV) != 1) {
        error = "EVP_EncryptInit_ex failed";
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    // Encrypt the data
    if (EVP_EncryptUpdate(ctx, ciphertext.data(), &len1, plaintext, static_cast<int>(plaintext_len)) != 1) {
        error = "EVP_EncryptUpdate failed";
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    // Finalize encryption (adds PKCS#7 padding)
    if (EVP_EncryptFinal_ex(ctx, ciphertext.data() + len1, &len2) != 1) {
        error = "EVP_EncryptFinal_ex failed";
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    // Clean up context
    EVP_CIPHER_CTX_free(ctx);

    // Resize vector to actual encrypted length
    ciphertext.resize(len1 + len2);
    return ciphertext;
}

std::vector<std::uint8_t> decrypt_aes_cbc(
    const std::uint8_t* ciphertext,
    std::size_t ciphertext_len,
    std::string& error
) {
    // Create EVP cipher context
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        error = "Failed to create EVP cipher context";
        return {};
    }

    // Allocate buffer for decrypted data
    // Maximum size is ciphertext length + block size
    int block_size = EVP_CIPHER_block_size(EVP_aes_256_cbc());
    std::vector<std::uint8_t> plaintext(ciphertext_len + block_size);

    int len1 = 0, len2 = 0;

    // Initialize decryption with AES-256-CBC
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, AES_KEY, AES_IV) != 1) {
        error = "EVP_DecryptInit_ex failed";
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    // Decrypt the data
    if (EVP_DecryptUpdate(ctx, plaintext.data(), &len1, ciphertext, static_cast<int>(ciphertext_len)) != 1) {
        error = "EVP_DecryptUpdate failed";
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    // Finalize decryption (validates and removes PKCS#7 padding)
    if (EVP_DecryptFinal_ex(ctx, plaintext.data() + len1, &len2) != 1) {
        // Decryption failure - likely wrong key, corrupted data, or invalid padding
        error = "EVP_DecryptFinal_ex failed - invalid padding or wrong key";
        EVP_CIPHER_CTX_free(ctx);
        return {};
    }

    // Clean up context
    EVP_CIPHER_CTX_free(ctx);

    // Resize vector to actual decrypted length
    plaintext.resize(len1 + len2);
    return plaintext;
}

}  // namespace quiz
