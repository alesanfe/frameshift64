// Shared between port.cpp (composition root) and port_init_phases.cpp
// (one-shot init phases). Not installed; internal to the application module.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
namespace Ship { class Context; class Console; }

std::shared_ptr<Ship::Context> PortContext();
std::string PortLocateFile(const std::string& basename);
int32_t ResetCommandHandler(std::shared_ptr<Ship::Console> console,
                            std::vector<std::string> args, std::string* output);

// The init phases keep the C linkage they had inside port.cpp's extern "C"
// block — PortInitImpl calls them from that block.
extern "C" {
void port_setenv(const char* name, const char* value);
void PortApplyCliEnvArgs(int argc, char* argv[]);
int  syNetReplayLoadDebugFile(const char* path);
int  PortInitTestHooks(int argc, char* argv[]);
void PortInitFeatureLatches(void);
int  PortInitCoreRuntime(void);
int  PortInitAssetsAndServices(void);
int  PortInitMods(void);
}
