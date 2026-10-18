#pragma once

#include <stdint.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include "ship/utils/binarytools/BinaryReader.h"

namespace tinyxml2 {
class XMLDocument;
class XMLElement;
} // namespace tinyxml2

namespace Ship {
#define OTR_HEADER_SIZE ((size_t)64)

struct File;
struct ResourceInitData;

/**
 * @brief Metadata block parsed from an archive's `manifest.json` file.
 *
 * Populated by `Archive::Load()` when a manifest.json is present in the
 * archive. Fields not present in the JSON are left as empty strings or
 * zero values. Schema mirrors upstream Kenix3/libultraship to remain
 * compatible with cross-port mod tooling.
 */
struct ArchiveManifest {
    std::string Name;        ///< Display name of the mod/archive.
    std::string Icon;        ///< Path to the mod's icon within the archive.
    std::string Author;      ///< Author(s) of the archive.
    std::string Version;     ///< Human-readable version string.
    std::string Website;     ///< Author's website URL.
    std::string Description; ///< Short description of the archive contents.
    std::string License;     ///< SPDX license identifier or URL.

    uint32_t CodeVersion = 0; ///< API version of compiled script modules bundled with this archive.
    uint32_t GameVersion = 0; ///< Numeric game version this archive targets.

    /** @brief Whether this archive can affect gameplay simulation (hitboxes,
     * physics, movesets). `false` declares a cosmetic-only mod (textures,
     * audio); such mods are excluded from the netplay compatibility hash.
     * Defaults to true — the conservative choice when the field is absent:
     * an undeclared archive is assumed to affect the simulation. This is a
     * mod-author contract, not a sandbox: a mod that lies and still shadows
     * gameplay data will desync netplay peers. */
    bool Simulation = true;

    std::string Main;                                      ///< Entry-point script path within the archive.
    std::unordered_map<std::string, std::string> Binaries; ///< Platform → binary path map for bundled native libs.
    std::vector<std::string> Dependencies;                 ///< Archive names that must be loaded before this one.

    /** @brief Declared capability names ("network", "persist", "debug",
     * "system"). The host gates sensitive symbol prefixes on this list:
     * a scripted mod resolving a gated symbol without the matching
     * capability gets a denied resolve. Absent key = unrestricted
     * (legacy mods); an explicit empty array = no gated access —
     * CapabilitiesDeclared distinguishes the two. */
    std::vector<std::string> Capabilities;
    bool CapabilitiesDeclared = false;

    std::string Checksum;  ///< Hex-encoded SHA-256 checksum of the archive contents.
    std::string Signature; ///< Base64-encoded digital signature over the checksum.
    std::string PublicKey; ///< Base64-encoded public key used to verify the signature.
};

class Archive : public std::enable_shared_from_this<Archive> {
    friend class ArchiveManager;

  public:
    Archive(const std::string& path);
    ~Archive();

    bool operator==(const Archive& rhs) const;

    void Load();
    void Unload();

    virtual std::shared_ptr<File> LoadFile(const std::string& filePath) = 0;
    virtual std::shared_ptr<File> LoadFile(uint64_t hash) = 0;
    std::shared_ptr<std::unordered_map<uint64_t, std::string>> ListFiles();
    std::shared_ptr<std::unordered_map<uint64_t, std::string>> ListFiles(const std::string& filter);
    bool HasFile(const std::string& filePath);
    bool HasFile(uint64_t hash);
    bool HasGameVersion();
    uint32_t GetGameVersion();

    /** @brief Returns the parsed archive manifest. Default-initialised if no manifest.json was present. */
    const ArchiveManifest& GetManifest();

    /** @brief Parses manifest.json content into `out` without mounting an
     * archive — the pure-parse entry point the fuzz harness and manifest
     * linters use. Returns true when the JSON parsed (unknown/mistyped
     * fields are ignored, matching ParseManifest's tolerant policy). */
    static bool ParseManifestBuffer(const char* data, size_t size, ArchiveManifest* out);

    /** @brief Verifies `signature` (minisign base64 SigStruct) over the
     * declared `checksum` with `public-key` (minisign base64 PubkeyStruct),
     * via the app-registered verifier — and only after IsChecksumValid()
     * passes, so the signature provably binds the shipped bytes. Returns
     * false when no verifier is registered or any field is missing. */
    bool IsSigned();

    /** @brief Recomputes the archive content checksum and compares it to
     * the manifest's `checksum` field ("fnv1a64:<16hex>" — FNV-1a over
     * name+size+bytes of every file except manifest.json, name-sorted). */
    bool IsChecksumValid();

    /** @brief Signature verifier the application registers at boot (keeps
     * ed25519 out of the engine). Receives (message, signature_b64,
     * pubkey_b64); returns true on a valid signature. */
    using SignatureVerifierFn =
        bool (*)(const std::string& message, const std::string& signature_b64,
                 const std::string& pubkey_b64);
    static void SetSignatureVerifier(SignatureVerifierFn fn);

    const std::string& GetPath();
    bool IsLoaded();

    virtual bool Open() = 0;
    virtual bool Close() = 0;
    virtual bool WriteFile(const std::string& filename, const std::vector<uint8_t>& data) = 0;

  protected:
    void SetLoaded(bool isLoaded);
    void SetGameVersion(uint32_t gameVersion);
    void IndexFile(const std::string& filePath);
    /** @brief Parses manifest.json into mManifest. Called by Load(). No-op if no manifest is present. */
    void ParseManifest();

  private:
    bool mIsLoaded;
    bool mHasGameVersion;
    uint32_t mGameVersion;
    std::string mPath;
    ArchiveManifest mManifest;
    std::shared_ptr<std::unordered_map<uint64_t, std::string>> mHashes;
};
} // namespace Ship
