#include "FileScanner.h"

#include <openssl/evp.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace modrinth_cli::files {
namespace {
std::string toLowerCopy(std::string input) {
    std::transform(input.begin(), input.end(), input.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return input;
}

bool isLikelyModFile(const std::filesystem::path& file_path) {
    const std::string extension = toLowerCopy(file_path.extension().string());
    return extension == ".jar" || extension == ".zip";
}
}  // namespace

std::string FileHasher::getSha512(const std::filesystem::path& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open file: " + file_path.string());
    }

    EVP_MD* digest = EVP_MD_fetch(nullptr, "SHA512", nullptr);
    if (digest == nullptr) {
        throw std::runtime_error("EVP_MD_fetch failed for SHA512");
    }

    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr) {
        EVP_MD_free(digest);
        throw std::runtime_error("EVP_MD_CTX_new failed");
    }

    if (EVP_DigestInit_ex2(context, digest, nullptr) != 1) {
        EVP_MD_CTX_free(context);
        EVP_MD_free(digest);
        throw std::runtime_error("EVP_DigestInit_ex2 failed");
    }

    std::vector<unsigned char> buffer(8192);
    while (file) {
        file.read(reinterpret_cast<char*>(buffer.data()),
                  static_cast<std::streamsize>(buffer.size()));
        const std::streamsize bytes_read = file.gcount();
        if (bytes_read > 0) {
            if (EVP_DigestUpdate(context, buffer.data(), static_cast<size_t>(bytes_read)) != 1) {
                EVP_MD_CTX_free(context);
                EVP_MD_free(digest);
                throw std::runtime_error("EVP_DigestUpdate failed");
            }
        }
    }

    unsigned char hash_bytes[EVP_MAX_MD_SIZE];
    unsigned int hash_length = 0;
    if (EVP_DigestFinal_ex(context, hash_bytes, &hash_length) != 1) {
        EVP_MD_CTX_free(context);
        EVP_MD_free(digest);
        throw std::runtime_error("EVP_DigestFinal_ex failed");
    }

    EVP_MD_CTX_free(context);
    EVP_MD_free(digest);

    std::ostringstream hash_stream;
    hash_stream << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < hash_length; ++index) {
        hash_stream << std::setw(2) << static_cast<int>(hash_bytes[index]);
    }

    return hash_stream.str();
}

std::vector<FileHashRecord> FileScanner::collectModFileHashes(
    const std::filesystem::path& mods_path) {
    if (!std::filesystem::exists(mods_path)) {
        throw std::runtime_error("Mods path does not exist: " + mods_path.string());
    }

    if (!std::filesystem::is_directory(mods_path)) {
        throw std::runtime_error("Mods path is not a directory: " + mods_path.string());
    }

    std::vector<FileHashRecord> records;
    for (const auto& entry : std::filesystem::directory_iterator(mods_path)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        const auto file_path = entry.path();
        if (!isLikelyModFile(file_path)) {
            continue;
        }

        records.push_back({std::filesystem::absolute(file_path), FileHasher::getSha512(file_path)});
    }

    return records;
}

}  // namespace modrinth_cli::files
