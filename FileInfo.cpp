#include "FileInfo.h"

FileInfo::FileInfo(std::string url, std::string filename, const bool& primary,
                   const std::optional<int64_t>& size, const std::optional<std::string>& file_type)
    : url_(std::move(url)),
      filename_(std::move(filename)),
      primary_(primary),
      size_(size),
      file_type_(file_type) {}