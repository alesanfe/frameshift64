#include "ship/resource/archive/O2rArchive.h"

#include "ship/Context.h"
#include "ship/window/Window.h"
#include "spdlog/spdlog.h"
#if defined(_MSC_VER) && defined(_DEBUG)
#include <cstdio>
#include <cstdlib>
#include <windows.h>
#endif

namespace Ship {
O2rArchive::O2rArchive(const std::string& archivePath) : Archive(archivePath) {
}

O2rArchive::~O2rArchive() {
    SPDLOG_INFO("destruct o2rarchive: {}", GetPath());
    Close();
}

// MSVC debug-only instrumentation for the shutdown heap-corruption crash:
// validate that mZipArchive's own allocation block is intact on the shared
// (ucrtbased) debug heap before handing it to zip_close(). If the block was
// stomped by an out-of-bounds writer elsewhere, the validator reports false
// BEFORE the free inside libzip trips 0xC0000374. Enabled at runtime with
// SSB64_HEAP_CHECK — no cost in normal or release runs.
#if defined(_MSC_VER) && defined(_DEBUG)
static bool O2rSharedHeapCheck(const char* tag, const zip_t* za) {
    if (std::getenv("SSB64_HEAP_CHECK") == nullptr) {
        return true;
    }
    HMODULE ucrt = GetModuleHandleW(L"ucrtbased.dll");
    int ptrOk = -1, heapOk = -1;
    if (ucrt != nullptr) {
        using IsValidPtrFn = int (*)(const void*);
        using CheckMemFn = int (*)();
        auto isValidPtr = reinterpret_cast<IsValidPtrFn>(GetProcAddress(ucrt, "_CrtIsValidHeapPointer"));
        auto checkMem = reinterpret_cast<CheckMemFn>(GetProcAddress(ucrt, "_CrtCheckMemory"));
        ptrOk = isValidPtr ? isValidPtr(za) : -1;
        heapOk = checkMem ? checkMem() : -1;
    }
    // Direct fflush'd write — spdlog output is lost when the process dies in
    // the very call we are instrumenting.
    if (FILE* f = std::fopen("heap_probe.log", "a")) {
        std::fprintf(f, "heap-check %s: archive=%p heap_ok=%d block_ok=%d\n", tag,
                     static_cast<const void*>(za), heapOk, ptrOk);
        std::fclose(f);
    }
    SPDLOG_INFO("heap-check {}: archive={} heap_ok={} block_ok={}", tag, static_cast<const void*>(za), heapOk,
                ptrOk);
    return ptrOk != 0;
}
#else
static bool O2rSharedHeapCheck(const char*, const zip_t*) {
    return true;
}
#endif

std::shared_ptr<File> O2rArchive::LoadFile(uint64_t hash) {
    const std::string* filePath =
        Context::GetInstance()->GetResourceManager()->GetArchiveManager()->HashToString(hash);
    if (filePath == nullptr) {
        return nullptr;
    }
    return LoadFile(*filePath);
}

std::shared_ptr<File> O2rArchive::LoadFile(const std::string& filePath) {
    const std::lock_guard<std::mutex> lock(mMutex);
    if (mZipArchive == nullptr) {
        SPDLOG_TRACE("Failed to open file {} from zip archive {}. Archive not open.", filePath, GetPath());
        return nullptr;
    }

    auto zipEntryIndex = zip_name_locate(mZipArchive, filePath.c_str(), 0);
    if (zipEntryIndex < 0) {
        SPDLOG_TRACE("Failed to find file {} in zip archive  {}.", filePath, GetPath());
        return nullptr;
    }

    struct zip_stat zipEntryStat;
    zip_stat_init(&zipEntryStat);
    if (zip_stat_index(mZipArchive, zipEntryIndex, 0, &zipEntryStat) != 0) {
        SPDLOG_TRACE("Failed to get entry information for file {} in zip archive  {}.", filePath, GetPath());
        return nullptr;
    }

    // Filesize 0, no logging needed
    if (zipEntryStat.size == 0) {
        SPDLOG_TRACE("Failed to load file {}; filesize 0", filePath, GetPath());
        return nullptr;
    }

    struct zip_file* zipEntryFile = zip_fopen_index(mZipArchive, zipEntryIndex, 0);
    if (!zipEntryFile) {
        SPDLOG_TRACE("Failed to open file {} in zip archive  {}.", filePath, GetPath());
        return nullptr;
    }

    auto fileToLoad = std::make_shared<File>();
    fileToLoad->Buffer = std::make_shared<std::vector<char>>(zipEntryStat.size);

    if (zip_fread(zipEntryFile, fileToLoad->Buffer->data(), zipEntryStat.size) < 0) {
        SPDLOG_TRACE("Error reading file {} in zip archive  {}.", filePath, GetPath());
    }

    if (zip_fclose(zipEntryFile) != 0) {
        SPDLOG_TRACE("Error closing file {} in zip archive  {}.", filePath, GetPath());
    }

    fileToLoad->IsLoaded = true;

    return fileToLoad;
}

bool O2rArchive::Open() {
    const std::lock_guard<std::mutex> lock(mMutex);
    mZipArchive = zip_open(GetPath().c_str(), ZIP_CREATE, nullptr);
    if (mZipArchive == nullptr) {
        SPDLOG_ERROR("Failed to load zip file \"{}\"", GetPath());
        return false;
    }

    auto zipNumEntries = zip_get_num_entries(mZipArchive, 0);
    for (auto i = 0; i < zipNumEntries; i++) {
        auto zipEntryName = zip_get_name(mZipArchive, i, 0);

        // It is possible for directories to have entries in a zip
        // file, we don't want those indexed as files in the archive
        if (zipEntryName[strlen(zipEntryName) - 1] == '/') {
            continue;
        }

        IndexFile(zipEntryName);
    }

    return true;
}

bool O2rArchive::Close() {
    const std::lock_guard<std::mutex> lock(mMutex);
    if (mZipArchive == nullptr) {
        SPDLOG_ERROR("Cannot close zip file. Zip file not loaded. \"{}\"", GetPath());
        return false;
    }

    O2rSharedHeapCheck("pre-zip_close", mZipArchive);

    if (zip_close(mZipArchive) == -1) {
        SPDLOG_ERROR("Failed to close zip file \"{}\"", GetPath());
        return false;
    }

    mZipArchive = nullptr;
    return true;
}

bool O2rArchive::WriteFile(const std::string& filePath, const std::vector<uint8_t>& data) {
    const std::lock_guard<std::mutex> lock(mMutex);
    if (!mZipArchive) {
        SPDLOG_ERROR("Cannot write to zip: Archive is not open.");
        return false;
    }

    // Create a new zip source from the data buffer
    zip_source_t* source = zip_source_buffer(mZipArchive, data.data(), data.size(), 0);
    if (!source) {
        SPDLOG_ERROR("Failed to create zip source for file \"{}\"", filePath);
        return false;
    }

    // Add or replace the file in the zip archive
    if (zip_file_add(mZipArchive, filePath.c_str(), source, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE) < 0) {
        SPDLOG_ERROR("Failed to add file \"{}\" to ZIP", filePath);
        zip_source_free(source);
        return false;
    }

    // Save changes to disk
    if (zip_close(mZipArchive) < 0) {
        zip_error_t* error = zip_get_error(mZipArchive);
        SPDLOG_ERROR("Failed to save changes to zip archive: {} ({})", zip_error_strerror(error),
                     zip_error_code_zip(error));
        zip_discard(mZipArchive); // Close zip and discard changes
        // zip_discard frees the archive — clear the handle or the destructor's
        // Close() would call zip_close() on freed memory.
        mZipArchive = nullptr;
        return false;
    }

    SPDLOG_INFO("Successfully wrote file: {}", filePath);

    // Reopen the zip file so that it may continued to be used by libultraship
    mZipArchive = zip_open(GetPath().c_str(), ZIP_CREATE, nullptr);
    if (mZipArchive == nullptr) {
        SPDLOG_ERROR("Failed to reopen zip file after writing.");
        return false;
    }

    IndexFile(filePath);

    // Success
    return true;
}

} // namespace Ship
