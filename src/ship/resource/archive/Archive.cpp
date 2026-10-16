#include "ship/resource/archive/Archive.h"

#include "spdlog/spdlog.h"

#include "ship/Context.h"
#include "ship/resource/File.h"
#include "ship/resource/ResourceLoader.h"
#include "ship/resource/ResourceType.h"
#include "ship/utils/binarytools/MemoryStream.h"
#include "ship/utils/glob.h"
#include "ship/utils/StrHash64.h"
#include "ship/window/Window.h"
#include <nlohmann/json.hpp>
#include <tinyxml2.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace Ship {
Archive::Archive(const std::string& path)
    : mIsLoaded(false), mHasGameVersion(false), mGameVersion(0xFFFFFFFF), mPath(path) {
    mHashes = std::make_shared<std::unordered_map<uint64_t, std::string>>();
}

Archive::~Archive() {
    SPDLOG_TRACE("destruct archive: {}", GetPath());
}

bool Archive::operator==(const Archive& rhs) const {
    return mPath == rhs.mPath;
}

void Archive::Load() {
    bool opened = Open();

    auto t = LoadFile("version");
    bool isGameVersionValid = false;
    if (t != nullptr && t->IsLoaded) {
        mHasGameVersion = true;
        auto stream = std::make_shared<MemoryStream>(t->Buffer->data(), t->Buffer->size());
        auto reader = std::make_shared<BinaryReader>(stream);
        Endianness endianness = (Endianness)reader->ReadUByte();
        reader->SetEndianness(endianness);
        SetGameVersion(reader->ReadUInt32());
        isGameVersionValid =
            Context::GetInstance()->GetResourceManager()->GetArchiveManager()->IsGameVersionValid(GetGameVersion());

        if (!isGameVersionValid) {
            SPDLOG_WARN("Attempting to load Archive \"{}\" with invalid version {}", GetPath(), GetGameVersion());
        }
    }

    if (opened) {
        ParseManifest();
    }

    SetLoaded(opened && (!mHasGameVersion || isGameVersionValid));

    if (!IsLoaded()) {
        Unload();
    }
}

void Archive::ParseManifest() {
    auto file = LoadFile("manifest.json");
    if (file == nullptr || !file->IsLoaded || file->Buffer == nullptr) {
        return;
    }

    const auto& buf = *file->Buffer;
    ArchiveManifest parsed;
    if (ParseManifestBuffer(buf.data(), buf.size(), &parsed)) {
        mManifest = std::move(parsed);
    } else {
        SPDLOG_WARN("Archive \"{}\" has malformed manifest.json", GetPath());
    }
}

bool Archive::ParseManifestBuffer(const char* data, size_t size, ArchiveManifest* out) {
    if (out == nullptr || data == nullptr || size == 0) {
        return false;
    }
    try {
        nlohmann::json j = nlohmann::json::parse(data, data + size);

        if (j.contains("name") && j["name"].is_string()) {
            out->Name = j["name"].get<std::string>();
        }
        if (j.contains("icon") && j["icon"].is_string()) {
            out->Icon = j["icon"].get<std::string>();
        }
        if (j.contains("author") && j["author"].is_string()) {
            out->Author = j["author"].get<std::string>();
        }
        if (j.contains("version") && j["version"].is_string()) {
            out->Version = j["version"].get<std::string>();
        }
        if (j.contains("website") && j["website"].is_string()) {
            out->Website = j["website"].get<std::string>();
        }
        if (j.contains("description") && j["description"].is_string()) {
            out->Description = j["description"].get<std::string>();
        }
        if (j.contains("license") && j["license"].is_string()) {
            out->License = j["license"].get<std::string>();
        }
        if (j.contains("code-version") && j["code-version"].is_number_integer()) {
            out->CodeVersion = j["code-version"].get<uint32_t>();
        }
        if (j.contains("game-version") && j["game-version"].is_number_integer()) {
            out->GameVersion = j["game-version"].get<uint32_t>();
        }
        if (j.contains("simulation") && j["simulation"].is_boolean()) {
            out->Simulation = j["simulation"].get<bool>();
        }
        if (j.contains("main") && j["main"].is_string()) {
            out->Main = j["main"].get<std::string>();
        }
        if (j.contains("binaries") && j["binaries"].is_object()) {
            for (auto& [key, value] : j["binaries"].items()) {
                if (value.is_string()) {
                    out->Binaries[key] = value.get<std::string>();
                }
            }
        }
        if (j.contains("dependencies") && j["dependencies"].is_array()) {
            for (const auto& dep : j["dependencies"]) {
                if (dep.is_string()) {
                    out->Dependencies.push_back(dep.get<std::string>());
                }
            }
        }
        if (j.contains("checksum") && j["checksum"].is_string()) {
            out->Checksum = j["checksum"].get<std::string>();
        }
        if (j.contains("signature") && j["signature"].is_string()) {
            out->Signature = j["signature"].get<std::string>();
        }
        if (j.contains("public-key") && j["public-key"].is_string()) {
            out->PublicKey = j["public-key"].get<std::string>();
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

const ArchiveManifest& Archive::GetManifest() {
    return mManifest;
}

namespace {
Archive::SignatureVerifierFn sSignatureVerifier = nullptr;

void fnv1a_accum(uint64_t& h, const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
}
} // namespace

void Archive::SetSignatureVerifier(SignatureVerifierFn fn) {
    sSignatureVerifier = fn;
}

bool Archive::IsSigned() {
    if (sSignatureVerifier == nullptr || mManifest.Signature.empty() ||
        mManifest.PublicKey.empty() || mManifest.Checksum.empty()) {
        return false;
    }
    /* The signature binds the *declared* checksum, so verify the bytes
     * actually match it first — otherwise a tampered archive could keep a
     * valid-looking signature while carrying different payloads. */
    if (!IsChecksumValid()) {
        return false;
    }
    return sSignatureVerifier(mManifest.Checksum, mManifest.Signature,
                              mManifest.PublicKey);
}

bool Archive::IsChecksumValid() {
    constexpr const char* kPrefix = "fnv1a64:";
    if (mManifest.Checksum.rfind(kPrefix, 0) != 0) {
        return false;
    }
    uint64_t h = 1469598103934665603ULL;   /* FNV-1a 64 offset basis */
    std::vector<std::string> names;
    names.reserve(mHashes->size());
    for (const auto& [hash, name] : *mHashes) {
        /* manifest.json carries the checksum itself — excluding it also
         * keeps the value stable across re-saves. */
        if (name != "manifest.json") {
            names.push_back(name);
        }
    }
    std::sort(names.begin(), names.end());
    for (const std::string& name : names) {
        auto file = LoadFile(name);
        if (file == nullptr || !file->IsLoaded || file->Buffer == nullptr) {
            return false;
        }
        const uint64_t sz = file->Buffer->size();
        fnv1a_accum(h, name.data(), name.size());
        fnv1a_accum(h, &sz, sizeof(sz));
        fnv1a_accum(h, file->Buffer->data(), file->Buffer->size());
    }
    char text[32];
    std::snprintf(text, sizeof(text), "%016llx",
                  static_cast<unsigned long long>(h));
    return mManifest.Checksum.substr(std::strlen(kPrefix)) == text;
}

void Archive::Unload() {
    Close();
    SetLoaded(false);
}

std::shared_ptr<std::unordered_map<uint64_t, std::string>> Archive::ListFiles() {
    return mHashes;
}

std::shared_ptr<std::unordered_map<uint64_t, std::string>> Archive::ListFiles(const std::string& filter) {
    auto result = std::make_shared<std::unordered_map<uint64_t, std::string>>();

    std::copy_if(mHashes->begin(), mHashes->end(), std::inserter(*result, result->begin()),
                 [filter](const std::pair<const int64_t, const std::string&> entry) {
                     return glob_match(filter.c_str(), entry.second.c_str());
                 });

    return result;
}

bool Archive::HasFile(const std::string& filePath) {
    return HasFile(CRC64(filePath.c_str()));
}

bool Archive::HasFile(uint64_t hash) {
    return mHashes->count(hash) > 0;
}

bool Archive::HasGameVersion() {
    return mHasGameVersion;
}

uint32_t Archive::GetGameVersion() {
    return mGameVersion;
}

const std::string& Archive::GetPath() {
    return mPath;
}

bool Archive::IsLoaded() {
    return mIsLoaded;
}

void Archive::SetLoaded(bool isLoaded) {
    mIsLoaded = isLoaded;
}

void Archive::SetGameVersion(uint32_t gameVersion) {
    mGameVersion = gameVersion;
}

void Archive::IndexFile(const std::string& filePath) {
    if (filePath.length() > 5 && filePath.substr(filePath.length() - 5) == ".meta") {
        IndexFile(filePath.substr(0, filePath.length() - 5));
        return;
    }

    (*mHashes)[CRC64(filePath.c_str())] = filePath;
}

std::shared_ptr<File> Archive::LoadFile(uint64_t hash) {
    const std::string* filePath =
        Context::GetInstance()->GetResourceManager()->GetArchiveManager()->HashToString(hash);
    if (filePath == nullptr) {
        return nullptr;
    }
    return LoadFile(*filePath);
}

} // namespace Ship
