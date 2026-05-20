#ifndef FILEINFO_H
#define FILEINFO_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>

class FileInfo {
    public:
        FileInfo(std::string url, std::string filename, const bool& primary,
                 const std::optional<int64_t>& size, const std::optional<std::string>& file_type);

        void addHash(const std::string& hash_method, const std::string& hash) {
            hashes_[hash_method] = hash;
        }

        void addURL(const std::string& url) { url_ = url; }

        const std::map<std::string, std::string>& getHashes() const { return hashes_; }

        const std::string& getUrl() const { return url_; }

        const std::string& getFilename() const { return filename_; }

        bool isPrimary() const { return primary_; }

        const std::optional<int64_t>& getSize() const { return size_; }

        const std::optional<std::string>& getFileType() const { return file_type_; }

    private:
        std::map<std::string, std::string> hashes_;
        std::string url_;
        std::string filename_;
        bool primary_ = false;
        std::optional<int64_t> size_;
        std::optional<std::string> file_type_;
};
#endif