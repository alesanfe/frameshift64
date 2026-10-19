/* gameloop_screenshot.cpp — SSB64_SCREENSHOT_FRAMES-driven backbuffer
 * capture. Extracted from gameloop.cpp: frame-list parsing
 * (comma-separated ints or "all") lives in port_screenshot_init_once;
 * each captured frame goes through portFastCaptureBackbufferPNG. */

#include "gameloop.h"
#include "port_log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_set>

extern "C" int portFastCaptureBackbufferPNG(const char *path);

static bool sScreenshotInited = false;
static bool sScreenshotEnabled = false;
static bool sScreenshotAllFrames = false;
static std::unordered_set<int> sScreenshotFrames;
static std::string sScreenshotDir;

static void port_screenshot_init_once(void)
{
	if (sScreenshotInited) {
		return;
	}
	sScreenshotInited = true;

	const char *frames_env = std::getenv("SSB64_SCREENSHOT_FRAMES");
	if (frames_env == nullptr || frames_env[0] == '\0') {
		sScreenshotEnabled = false;
		return;
	}

	const char *dir_env = std::getenv("SSB64_SCREENSHOT_DIR");
	sScreenshotDir = (dir_env != nullptr && dir_env[0] != '\0')
		? dir_env
		: "debug_traces/screenshots";

	if (std::strcmp(frames_env, "all") == 0) {
		sScreenshotAllFrames = true;
	} else {
		/* Parse comma-separated integer list. Ignores malformed tokens. */
		const char *p = frames_env;
		while (*p != '\0') {
			while (*p == ',' || *p == ' ' || *p == '\t') {
				p++;
			}
			if (*p == '\0') {
				break;
			}
			char *end = nullptr;
			long v = std::strtol(p, &end, 10);
			if (end != p && v >= 0 && v <= 0x7FFFFFFF) {
				sScreenshotFrames.insert(static_cast<int>(v));
			}
			if (end == p) {
				/* Couldn't parse — skip a char to avoid infinite loop. */
				p++;
			} else {
				p = end;
			}
		}
	}

	sScreenshotEnabled = sScreenshotAllFrames || !sScreenshotFrames.empty();

	if (sScreenshotEnabled) {
		std::error_code ec;
		std::filesystem::create_directories(sScreenshotDir, ec);
		if (ec) {
			port_log("SSB64: screenshot: failed to create dir '%s': %s\n",
				sScreenshotDir.c_str(), ec.message().c_str());
		}
		if (sScreenshotAllFrames) {
			port_log("SSB64: screenshot capture ENABLED (all frames) dir='%s'\n",
				sScreenshotDir.c_str());
		} else {
			port_log("SSB64: screenshot capture ENABLED (%zu frames) dir='%s'\n",
				sScreenshotFrames.size(), sScreenshotDir.c_str());
		}
	}
}

static void port_screenshot_maybe_capture(int frame)
{
	if (!sScreenshotEnabled) {
		return;
	}
	if (!sScreenshotAllFrames && sScreenshotFrames.count(frame) == 0) {
		return;
	}

	char path[1024];
	std::snprintf(path, sizeof(path), "%s/frame_%d.png",
		sScreenshotDir.c_str(), frame);

	int ok = portFastCaptureBackbufferPNG(path);
	if (ok) {
		port_log("SSB64: screenshot frame %d -> %s\n", frame, path);
	} else {
		port_log("SSB64: screenshot frame %d FAILED -> %s\n", frame, path);
	}
}

void PortScreenshotMaybeCaptureFrame(int frame)
{
	port_screenshot_init_once();
	port_screenshot_maybe_capture(frame);
}
