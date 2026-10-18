#include "ship/scripting/ScriptLoader.h"

#ifndef DISABLE_SCRIPTING

#include "ship/resource/ResourceManager.h"
#include "ship/resource/archive/Archive.h"
#include "ship/resource/File.h"
#include "spdlog/spdlog.h"
#include <optional>
#include <fstream>
#include <sstream>
#include <string_view>
#include <libtcc.h>
#include <algorithm>
#include <cctype>
#include <memory>
#include <queue>
#include <unordered_map>
#include <filesystem>

#include "ship/Context.h"
#include "ship/config/ConsoleVariable.h"

#if defined(_WIN32) || defined(__CYGWIN__)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace Ship {
struct ScriptLoader::LoadedScript {
    Scripting::LibraryLoader Loader;
    TCCState* State = nullptr;
    std::unordered_map<std::string, std::unique_ptr<void*>> ImportPointers;

    LoadedScript() = default;
    LoadedScript(const LoadedScript&) = delete;
    LoadedScript& operator=(const LoadedScript&) = delete;

    ~LoadedScript() {
        Unload();
    }

    void* GetFunction(const std::string& name) {
        if (State != nullptr) {
            return tcc_get_symbol(State, name.c_str());
        }
        return Loader.GetFunction(name);
    }

    void Unload() {
        if (State != nullptr) {
            tcc_delete(State);
            State = nullptr;
        }
        Loader.Unload();
        ImportPointers.clear();
    }
};

std::optional<std::vector<uint8_t>> LoadFromO2R(const std::string& path,
                                                const std::shared_ptr<Archive>& archive = nullptr) {
    const auto file = archive->LoadFile(path);
    if (file == nullptr || !file->IsLoaded) {
        SPDLOG_ERROR("Failed to load script file: {}", path);
        return std::nullopt;
    }

    return std::vector<uint8_t>(file->Buffer->begin(), file->Buffer->end());
}

constexpr std::string_view Trim(const std::string_view v) {
    constexpr std::string_view whitespace = " \t\r\n";

    const auto start = v.find_first_not_of(whitespace);
    if (start == std::string_view::npos) {
        return {};
    }

    const auto end = v.find_last_not_of(whitespace);
    return v.substr(start, end - start + 1);
}

constexpr std::string_view GetPlatform() {
#if defined(_WIN32) || defined(_WIN64)
#if defined(_M_ARM64) || defined(__aarch64__)
    return "windows_arm64";
#else
    return "windows_x64";
#endif

#elif defined(__APPLE__) || defined(__MACH__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
    return "ios";
#elif TARGET_OS_MAC
    return "darwin";
#endif

#elif defined(__ANDROID__)
    return "android";

#elif defined(__linux__)
#if defined(__x86_64__) || defined(_M_X64)
    return "linux_x64";
#elif defined(__i386__) || defined(_M_IX86)
    return "linux_x86";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "linux_arm64";
#else
    return "linux_generic";
#endif

#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    return "bsd";
#else
#error "Unsupported Operating System"
#endif
}

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::optional<std::filesystem::path> FindLibraryFile(const std::vector<std::string>& libraryPaths,
                                                     const std::string& library) {
    std::filesystem::path libraryPath(library);
    if (libraryPath.is_absolute() && std::filesystem::exists(libraryPath)) {
        return libraryPath;
    }

    if (std::filesystem::exists(libraryPath)) {
        return std::filesystem::absolute(libraryPath);
    }

    for (const std::string& searchPath : libraryPaths) {
        std::filesystem::path candidate = std::filesystem::path(searchPath) / libraryPath;
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }

    return std::nullopt;
}

void* ResolveHostSymbol(const std::string& name) {
#if defined(_WIN32) || defined(__CYGWIN__)
    return reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA(nullptr), name.c_str()));
#elif defined(__linux__) || defined(__APPLE__)
    return dlsym(RTLD_DEFAULT, name.c_str());
#else
    return nullptr;
#endif
}

std::optional<std::string> ParseDefExportName(std::string_view line) {
    line = Trim(line);
    if (line.empty() || line[0] == ';' || line[0] == '#') {
        return std::nullopt;
    }
    if (line.size() >= 2 && line[0] == '/' && line[1] == '/') {
        return std::nullopt;
    }

    const std::string lowerLine = ToLower(std::string(line));
    if (lowerLine == "exports" || lowerLine.rfind("library", 0) == 0) {
        return std::nullopt;
    }

    const size_t whitespace = line.find_first_of(" \t\r\n");
    std::string name(line.substr(0, whitespace));
    const size_t alias = name.find('=');
    if (alias != std::string::npos) {
        name.resize(alias);
    }
    const size_t ordinal = name.find('@');
    if (ordinal != std::string::npos) {
        name.resize(ordinal);
    }

    if (name.size() >= 2 && name.front() == '"' && name.back() == '"') {
        name = name.substr(1, name.size() - 2);
    }
    if (name.empty()) {
        return std::nullopt;
    }

    return name;
}

void RegisterImportPointer(TCCState* state, ScriptLoader::LoadedScript& module, const std::string& symbol, void* addr) {
#if defined(_WIN32) || defined(__CYGWIN__)
    const std::string importName = "__imp_" + symbol;
    auto slot = std::make_unique<void*>(addr);
    void** slotAddress = slot.get();
    auto [it, inserted] = module.ImportPointers.emplace(importName, std::move(slot));
    if (!inserted) {
        it->second = std::make_unique<void*>(addr);
        slotAddress = it->second.get();
    }
    tcc_add_symbol(state, it->first.c_str(), slotAddress);
#else
    (void)state;
    (void)module;
    (void)symbol;
    (void)addr;
#endif
}

void AddSymbolsFromDef(TCCState* state, ScriptLoader::LoadedScript& module, const std::filesystem::path& defPath) {
    std::ifstream def(defPath);
    if (!def.is_open()) {
        throw std::runtime_error("Failed to open TCC symbol definition file: " + defPath.string());
    }

    size_t resolved = 0;
    size_t missing = 0;
    std::string line;
    while (std::getline(def, line)) {
        auto exportName = ParseDefExportName(line);
        if (!exportName.has_value()) {
            continue;
        }

        void* addr = ResolveHostSymbol(exportName.value());
        if (addr == nullptr) {
            missing++;
            if (!exportName->empty() && exportName->front() != '?') {
                SPDLOG_WARN("TCC host symbol not found: {}", exportName.value());
            }
            continue;
        }

        tcc_add_symbol(state, exportName->c_str(), addr);
        RegisterImportPointer(state, module, exportName.value(), addr);
        resolved++;
    }

    SPDLOG_INFO("Registered {} TCC host symbols from {} ({} missing)", resolved, defPath.string(), missing);
}

void SetTccLibraryRoot(TCCState* state, const std::vector<std::string>& libraryPaths) {
    if (libraryPaths.empty()) {
        return;
    }

    std::filesystem::path tccRoot = std::filesystem::path(libraryPaths.front()).parent_path();
    if (tccRoot.empty() || !std::filesystem::exists(tccRoot)) {
        return;
    }

    const std::string tccRootString = tccRoot.string();
    tcc_set_lib_path(state, tccRootString.c_str());
}

ScriptLoader::~ScriptLoader() = default;

void ScriptLoader::Compile(const std::shared_ptr<Archive>& archive) {
    const ArchiveManifest& info = archive->GetManifest();
    constexpr std::string_view platform = GetPlatform();
    const bool isCodeMod = !info.Main.empty() || !info.Binaries.empty();

    if (mSafeLevel == SafeLevel::DISABLE_SCRIPTS) {
        SPDLOG_WARN("Script loading is disabled. Skipping script from archive: {}", archive->GetPath());
        return;
    }

    if (!isCodeMod) {
        return;
    }

    if (info.CodeVersion != mCodeVersion) {
        SPDLOG_ERROR("Incompatible code version for archive {}: expected {}, got {}", archive->GetPath(), mCodeVersion,
                     info.CodeVersion);
        return;
    }

    const bool isTrusted = archive->IsSigned() && archive->IsChecksumValid();

    if (mSafeLevel == SafeLevel::ONLY_TRUSTED_SCRIPTS && !isTrusted) {
        throw std::runtime_error(
            "Script loading is disabled for untrusted scripts. Failed to load script from archive: " +
            archive->GetPath());
    }

    if (mSafeLevel == SafeLevel::WARN_UNTRUSTED_SCRIPTS) {
        if (isTrusted) {
            SPDLOG_INFO("Loaded trusted script from archive: {}", archive->GetPath());
        } else {
            SPDLOG_WARN(
                "Loaded untrusted script from archive: {}. This script is not signed or has an invalid checksum. "
                "This may be a security risk if you do not trust the source of this script.",
                archive->GetPath());
        }
    }

    mLoadedArchives.push_back(archive);

    auto module = std::make_shared<LoadedScript>();

    const auto& binaries = info.Binaries;

    if (binaries.contains(std::string(platform))) {
        const std::string temp = module->Loader.GenerateTempFile();
        const std::string& path = binaries.at(std::string(platform));
        auto data = LoadFromO2R(path, archive);
        if (!data.has_value()) {
            throw std::runtime_error("Failed to load platform-specific binary: " + path);
        }

        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            throw std::runtime_error("Failed to create temporary file for platform-specific binary: " + temp);
        }
        out.write(reinterpret_cast<const char*>(data->data()), data->size());
        out.close();
        module->Loader.Init(temp);
    } else if (!info.Main.empty()) {
        const auto data = LoadFromO2R(info.Main, archive);
        if (!data.has_value()) {
            throw std::runtime_error("Failed to load main script: " + info.Main);
        }

        TCCState* s = tcc_new();
        if (!s) {
            throw std::runtime_error("Failed to create TCCState");
        }

        tcc_set_error_func(s, nullptr, [](void* opaque, const char* msg) {
            std::string_view sv(msg);
            if (sv.find("warning") != std::string_view::npos) {
                SPDLOG_WARN("Compiler: {}", msg);
            } else if (sv.find("error") != std::string_view::npos || sv.find("fatal") != std::string_view::npos) {
                SPDLOG_ERROR("Compiler: {}", msg);
            } else {
                SPDLOG_INFO("Compiler: {}", msg);
            }
        });

        tcc_define_symbol(s, "__DLL__", "1");
        tcc_define_symbol(s, "LUS_TCC_MEMORY", "1");

        for (const auto& [key, value] : mCompileDefines) {
            tcc_define_symbol(s, key.c_str(), value.c_str());
        }

        tcc_set_options(s, mBuildOptions.c_str());
        SetTccLibraryRoot(s, mLibraryPaths);
        tcc_set_output_type(s, TCC_OUTPUT_MEMORY);

        for (const std::string& includePath : mIncludePaths) {
            if (!std::filesystem::exists(includePath)) {
                SPDLOG_WARN("Include path does not exist: {}", includePath);
                continue;
            }

            if (!std::filesystem::is_directory(includePath)) {
                SPDLOG_WARN("Include path is not a directory: {}", includePath);
                continue;
            }

            tcc_add_include_path(s, includePath.c_str());
        }

        for (const std::string& libraryPath : mLibraryPaths) {
            if (!std::filesystem::exists(libraryPath)) {
                SPDLOG_WARN("Library path does not exist: {}", libraryPath);
                continue;
            }

            if (!std::filesystem::is_directory(libraryPath)) {
                SPDLOG_WARN("Library path is not a directory: {}", libraryPath);
                continue;
            }

            tcc_add_library_path(s, libraryPath.c_str());
        }

        for (const std::string& library : mLibraries) {
            const std::string extension = ToLower(std::filesystem::path(library).extension().string());
            if (extension == ".def") {
                auto defPath = FindLibraryFile(mLibraryPaths, library);
                if (!defPath.has_value()) {
                    tcc_delete(s);
                    throw std::runtime_error("Missing TCC symbol definition file: " + library);
                }
                AddSymbolsFromDef(s, *module, defPath.value());
                continue;
            }

            if (tcc_add_library(s, library.c_str()) == -1) {
                tcc_delete(s);
                throw std::runtime_error("Failed to add TCC library: " + library);
            }
        }

        const std::vector<uint8_t>& raw = data.value();
        std::string content(raw.begin(), raw.end());
        std::istringstream stream(content);
        std::string line;

        while (std::getline(stream, line)) {
            if (line.empty()) {
                continue;
            }

            line.erase(line.find_last_not_of(" \r\n\t") + 1);
            line.erase(0, line.find_first_not_of(" \r\n\t"));

            if (line.empty() || line[0] == '#') {
                continue;
            }

            std::string safePath = line;

            auto buf = LoadFromO2R(safePath, archive);
            if (!buf.has_value()) {
                tcc_delete(s);
                throw std::runtime_error("Failed to load script file: '" + safePath + "'");
            }

            std::string lineFixer = "#line 1 \"[" + info.Name + "]:" + safePath + "\"\n";
            std::string sourceCode = lineFixer + std::string(buf->begin(), buf->end());
            if (tcc_compile_string(s, sourceCode.c_str()) == -1) {
                tcc_delete(s);
                throw std::runtime_error("TCC Error in " + safePath);
            }
        }

        if (tcc_relocate(s) < 0) {
            tcc_delete(s);
            throw std::runtime_error("Failed to relocate compiled code for " + info.Name);
        }

        module->State = s;
    }

    mLoadedScripts[info.Name] = std::move(module);
};

void ScriptLoader::CompileAll(const std::optional<std::function<void(const std::shared_ptr<Archive>&)>>& preCallback,
                              const std::optional<std::function<void()>>& postCallback) {
    auto archive = Context::GetInstance()->GetResourceManager()->GetArchiveManager();
    auto list = archive->GetArchives();

    for (const auto& entry : *list) {
        const auto& info = entry->GetManifest();
        if (info.Main.empty() && info.Binaries.empty()) {
            continue;
        }

        /* API-version gate: a mod compiled against a newer script API than
         * this build supports is skipped rather than loaded-and-broken.
         * code-version 0/absent predates the field and loads unguarded. */
        constexpr uint32_t kMaxSupportedCodeVersion = 1;
        if (info.CodeVersion > kMaxSupportedCodeVersion) {
            SPDLOG_WARN("Skipping script {} ({}): manifest code-version {} exceeds "
                        "this build's script API {} — rebuild the mod or update the game.",
                        info.Name, entry->GetPath(), info.CodeVersion, kMaxSupportedCodeVersion);
            continue;
        }

        if (preCallback.has_value()) {
            preCallback.value()(entry);
        }
        Compile(entry);
        if (postCallback.has_value()) {
            postCallback.value()();
        }
    }
}

std::vector<std::string> ScriptLoader::GetLoadersInDependencyOrder() const {
    std::vector<std::string> orderedLoaders;

    std::vector<std::string> loadOrder;
    std::unordered_map<std::string, std::shared_ptr<Archive>> archiveMap;
    std::unordered_map<std::string, int> inDegree;
    std::unordered_map<std::string, std::vector<std::string>> dependents;

    for (const auto& entry : mLoadedArchives) {
        const auto& info = entry->GetManifest();

        if (!mLoadedScripts.contains(info.Name)) {
            continue;
        }

        archiveMap[info.Name] = entry;
        inDegree[info.Name] = 0;
    }

    for (const auto& [name, entry] : archiveMap) {
        const auto& deps = entry->GetManifest().Dependencies;
        for (const std::string& depName : deps) {
            if (archiveMap.contains(depName)) {
                dependents[depName].push_back(name);
                inDegree[name]++;
            } else {
                throw std::runtime_error("Loaded archive " + name +
                                         " depends on missing or unloaded archive: " + depName);
            }
        }
    }

    std::queue<std::string> readyQueue;
    for (const auto& [name, degree] : inDegree) {
        if (degree == 0) {
            readyQueue.push(name);
        }
    }

    while (!readyQueue.empty()) {
        std::string current = readyQueue.front();
        readyQueue.pop();

        loadOrder.push_back(current);

        for (const std::string& dependentName : dependents[current]) {
            inDegree[dependentName]--;

            if (inDegree[dependentName] == 0) {
                readyQueue.push(dependentName);
            }
        }
    }

    if (loadOrder.size() != archiveMap.size()) {
        throw std::runtime_error("Circular dependency detected among loaded scripts. Failed to resolve init order.");
    }

    for (const std::string& name : loadOrder) {
        orderedLoaders.push_back(name);
    }

    return orderedLoaders;
}

void ScriptLoader::LoadAll(
    const std::optional<std::function<void(const std::string&)>>& preInitCb,
    const std::optional<std::function<void(const std::string&)>>& postInitCb) {
    auto loaders = GetLoadersInDependencyOrder();
    for (const auto& id : loaders) {
        auto& loader = *mLoadedScripts.at(id);
        SPDLOG_INFO("Initializing script: {}", id);
        const auto init = (Scripting::LibraryFunc_t)loader.GetFunction("ModInit");
        if (preInitCb.has_value()) {
            preInitCb.value()(id);
        }
        if (init) {
            init();
        }
        if (postInitCb.has_value()) {
            postInitCb.value()(id);
        }
    }
}
void ScriptLoader::UnloadAll(
    const std::optional<std::function<void(const std::string&)>>& preExitCb,
    const std::optional<std::function<void(const std::string&)>>& postExitCb) {
    auto loaders = GetLoadersInDependencyOrder();
    for (auto it = loaders.rbegin(); it != loaders.rend(); ++it) {
        auto& loader = *mLoadedScripts.at(*it);
        SPDLOG_INFO("Uninitialize script: {}", *it);
        const auto exit = (Scripting::LibraryFunc_t)loader.GetFunction("ModExit");

        if (preExitCb.has_value()) {
            preExitCb.value()(*it);
        }
        if (exit) {
            exit();
        }
        /* postExitCb fires BEFORE loader.Unload() so the host can walk
         * per-mod state (uninstall hooks etc.) while the mod's code is
         * still mapped. */
        if (postExitCb.has_value()) {
            postExitCb.value()(*it);
        }

        loader.Unload();
    }
    mLoadedScripts.clear();
}

void* ScriptLoader::GetFunction(const std::string& name, const std::string& function) {
    if (mLoadedScripts.contains(name)) {
        return mLoadedScripts.at(name)->GetFunction(function);
    }
    return nullptr;
};

} // namespace Ship

#endif // DISABLE_SCRIPTING
