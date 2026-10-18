#ifdef __EMSCRIPTEN__
#include "Companion.h"

#include <emscripten/bind.h>
using namespace emscripten;

/* Companion::Instance is defined in Companion.cpp on this fork — only
 * assign it here; defining it again is a duplicate symbol at link. */

void Bind(Companion* instance) {
    Companion::Instance = instance;
}

/* Companion::Process takes a std::atomic<size_t>& progress counter that JS
 * cannot produce — hide it behind a member-shaped free function. The
 * counter is a plain progress cell (no cancellation semantics on this
 * fork), so a per-call static is safe: extraction runs once per page load. */
static void CompanionProcess(Companion* self) {
    static std::atomic<size_t> sCount{0};
    sCount = 0;
    self->Process(sCount);
}

EMSCRIPTEN_BINDINGS(Torch) {
    register_vector<uint8_t>("VectorUInt8");

    function("Bind", &Bind, allow_raw_pointers());

    enum_<ExportType>("ExportType")
        .value("Header", ExportType::Header)
        .value("Code", ExportType::Code)
        .value("Binary", ExportType::Binary)
        .value("Modding", ExportType::Modding)
        .value("XML", ExportType::XML);

    enum_<ArchiveType>("ArchiveType")
        .value("None", ArchiveType::None)
        .value("OTR", ArchiveType::OTR)
        .value("O2R", ArchiveType::O2R);

    enum_<N64::CountryCode>("CountryCode")
        .value("Japan", N64::CountryCode::Japan)
        .value("NorthAmerica", N64::CountryCode::NorthAmerica)
        .value("Europe", N64::CountryCode::Europe)
        .value("Unknown", N64::CountryCode::Unknown);

    class_<N64::Cartridge>("Cartridge")
        .function("GetGameTitle", &N64::Cartridge::GetGameTitle)
        .function("GetCountryCode", &N64::Cartridge::GetCountryCode)
        .function("GetCountry", &N64::Cartridge::GetCountry)
        .function("GetVersion", &N64::Cartridge::GetVersion)
        .function("GetHash", &N64::Cartridge::GetHash)
        .function("GetCRC", &N64::Cartridge::GetCRC);

    class_<Companion>("Companion")
        .constructor<std::vector<uint8_t>, ArchiveType, bool, bool, std::string, std::string>()
        .constructor<std::vector<uint8_t>, ArchiveType, bool, bool, std::string>()
        .constructor<std::vector<uint8_t>, ArchiveType, bool, bool>()
        .function("Init", select_overload<void(ExportType)>(&Companion::Init))
        .function("GetCartridge", &Companion::GetCartridge, allow_raw_pointers())
        .function("Process", &CompanionProcess, allow_raw_pointer<arg<0>>())
        .function("GetRomData", &Companion::GetRomData, allow_raw_pointers());
}
#endif