#pragma once

#undef _DLL

#include <string>
#include <stdint.h>
#include <string>
#include <mutex>

#include "zip.h"

#include "ship/resource/File.h"
#include "ship/resource/Resource.h"
#include "ship/resource/archive/Archive.h"

namespace Ship {
struct File;

class O2rArchive final : virtual public Archive {
  public:
    O2rArchive(const std::string& archivePath);
    ~O2rArchive();

    bool Open();
    bool Close();
    bool WriteFile(const std::string& filename, const std::vector<uint8_t>& data);

    std::shared_ptr<File> LoadFile(const std::string& filePath);
    std::shared_ptr<File> LoadFile(uint64_t hash);

  private:
    zip_t* mZipArchive;
    // libzip is not thread-safe per archive: concurrent zip_* calls on the
    // same zip_t corrupt its internal state (observed as heap corruption
    // inside zip.dll during zip_close at shutdown). The ResourceManager
    // thread pool calls LoadFile from several workers, so every entry point
    // touching mZipArchive serializes on this mutex.
    std::mutex mMutex;
};
} // namespace Ship
