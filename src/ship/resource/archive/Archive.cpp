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

    try {
        nlohmann::json j = nlohmann::json::parse(file->Buffer->begin(), file->Buffer->end());

        if (j.contains("name") && j["name"].is_string()) {
            mManifest.Name = j["name"].get<std::string>();
        }
        if (j.contains("icon") && j["icon"].is_string()) {
            mManifest.Icon = j["icon"].get<std::string>();
        }
        if (j.contains("author") && j["author"].is_string()) {
            mManifest.Author = j["author"].get<std::string>();
        }
        if (j.contains("version") && j["version"].is_string()) {
            mManifest.Version = j["version"].get<std::string>();
        }
        if (j.contains("website") && j["website"].is_string()) {
            mManifest.Website = j["website"].get<std::string>();
        }
        if (j.contains("description") && j["description"].is_string()) {
            mManifest.Description = j["description"].get<std::string>();
        }
        if (j.contains("license") && j["license"].is_string()) {
            mManifest.License = j["license"].get<std::string>();
        }
        if (j.contains("code-version") && j["code-version"].is_number_integer()) {
            mManifest.CodeVersion = j["code-version"].get<uint32_t>();
        }
        if (j.contains("game-version") && j["game-version"].is_number_integer()) {
            mManifest.GameVersion = j["game-version"].get<uint32_t>();
        }
        if (j.contains("simulation") && j["simulation"].is_boolean()) {
            mManifest.Simulation = j["simulation"].get<bool>();
        }
        if (j.contains("main") && j["main"].is_string()) {
            mManifest.Main = j["main"].get<std::string>();
        }
        if (j.contains("binaries") && j["binaries"].is_object()) {
            for (auto& [key, value] : j["binaries"].items()) {
                if (value.is_string()) {
                    mManifest.Binaries[key] = value.get<std::string>();
                }
            }
        }
        if (j.contains("dependencies") && j["dependencies"].is_array()) {
            for (const auto& dep : j["dependencies"]) {
                if (dep.is_string()) {
                    mManifest.Dependencies.push_back(dep.get<std::string>());
                }
            }
        }
        if (j.contains("checksum") && j["checksum"].is_string()) {
            mManifest.Checksum = j["checksum"].get<std::string>();
        }
        if (j.contains("signature") && j["signature"].is_string()) {
            mManifest.Signature = j["signature"].get<std::string>();
        }
        if (j.contains("public-key") && j["public-key"].is_string()) {
            mManifest.PublicKey = j["public-key"].get<std::string>();
        }
    } catch (const std::exception& e) {
        SPDLOG_WARN("Archive \"{}\" has malformed manifest.json: {}", GetPath(), e.what());
    }
}

const ArchiveManifest& Archive::GetManifest() {
    return mManifest;
}

bool Archive::IsSigned() {
    // TODO: implement signature verification (mManifest.Signature + .PublicKey + .Checksum).
    return false;
}

bool Archive::IsChecksumValid() {
    // TODO: implement checksum verification (recompute + compare against mManifest.Checksum).
    return false;
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
    const std::string& filePath =
        *Context::GetInstance()->GetResourceManager()->GetArchiveManager()->HashToString(hash);
    return LoadFile(filePath);
}

} // namespace Ship
