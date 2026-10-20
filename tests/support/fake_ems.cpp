/* fake_ems.cpp — in-process implementation of the emscripten WebSocket
 * API shim (tests/support/emscripten_shim/). Sockets are records in a
 * map; browser events are delivered explicitly via fake_ems::Fire* so
 * tests control ordering deterministically. Resource accounting
 * (new/delete/close counts) turns handle-balance regressions into test
 * failures. */
#include "fake_ems.h"

#include <emscripten.h>
#include <emscripten/console.h>
#include <emscripten/websocket.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#if defined(_WIN32)
#define popen _popen
#define pclose _pclose
#define strdup _strdup
#endif

namespace {

struct FakeSock {
    int id = 0;
    std::string url;
    bool deleted = false;
    bool sendFails = false;
    void *userOpen = nullptr;
    em_socket_open_callback onOpen = nullptr;
    void *userError = nullptr;
    em_socket_error_callback onError = nullptr;
    void *userClose = nullptr;
    em_socket_close_callback onClose = nullptr;
    void *userMsg = nullptr;
    em_socket_message_callback onMsg = nullptr;
    size_t sentBin = 0;
    size_t sentTxt = 0;
};

double sNow = 0.0;
int sNextId = 1;
int sNewCount = 0;
int sDeleteCount = 0;
int sCloseCount = 0;
int sLast = 0;
bool sSupported = true;
bool sNextNewFails = false;
const char *sScriptResult = nullptr;
std::map<int, FakeSock> sSocks;

/* JS string-literal quoting for embedding a value into the node runner
 * below — mirrors JSON string escaping for the scalar cases. */
std::string js_quote(const char *s)
{
    std::string out = "\"";
    for (const char *p = s; *p != '\0'; ++p) {
        switch (*p) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += *p;
        }
    }
    return out + "\"";
}

FakeSock *sock(int id)
{
    auto it = sSocks.find(id);
    return (it != sSocks.end() && !it->second.deleted) ? &it->second
                                                     : nullptr;
}

} // namespace

extern "C" double emscripten_get_now(void) { return sNow; }

extern "C" EMSCRIPTEN_WEBSOCKET_T
emscripten_websocket_new(EmscriptenWebSocketCreateAttributes *attrs)
{
    ++sNewCount;
    if (sNextNewFails) {
        sNextNewFails = false;
        return 0;
    }
    FakeSock &s = sSocks[sNextId];
    s = FakeSock{};
    s.id = sNextId;
    s.url = (attrs != nullptr && attrs->url != nullptr) ? attrs->url : "";
    sLast = sNextId++;
    return s.id;
}

extern "C" EMSCRIPTEN_RESULT
emscripten_websocket_set_onopen_callback(EMSCRIPTEN_WEBSOCKET_T ws,
                                         void *user, em_socket_open_callback cb)
{
    FakeSock *s = sock(ws);
    if (s == nullptr) {
        return EMSCRIPTEN_RESULT_INVALID_PARAM;
    }
    s->userOpen = user;
    s->onOpen = cb;
    return EMSCRIPTEN_RESULT_SUCCESS;
}

extern "C" EMSCRIPTEN_RESULT
emscripten_websocket_set_onerror_callback(EMSCRIPTEN_WEBSOCKET_T ws,
                                          void *user, em_socket_error_callback cb)
{
    FakeSock *s = sock(ws);
    if (s == nullptr) {
        return EMSCRIPTEN_RESULT_INVALID_PARAM;
    }
    s->userError = user;
    s->onError = cb;
    return EMSCRIPTEN_RESULT_SUCCESS;
}

extern "C" EMSCRIPTEN_RESULT
emscripten_websocket_set_onclose_callback(EMSCRIPTEN_WEBSOCKET_T ws,
                                          void *user, em_socket_close_callback cb)
{
    FakeSock *s = sock(ws);
    if (s == nullptr) {
        return EMSCRIPTEN_RESULT_INVALID_PARAM;
    }
    s->userClose = user;
    s->onClose = cb;
    return EMSCRIPTEN_RESULT_SUCCESS;
}

extern "C" EMSCRIPTEN_RESULT
emscripten_websocket_set_onmessage_callback(
    EMSCRIPTEN_WEBSOCKET_T ws, void *user, em_socket_message_callback cb)
{
    FakeSock *s = sock(ws);
    if (s == nullptr) {
        return EMSCRIPTEN_RESULT_INVALID_PARAM;
    }
    s->userMsg = user;
    s->onMsg = cb;
    return EMSCRIPTEN_RESULT_SUCCESS;
}

extern "C" EMSCRIPTEN_RESULT
emscripten_websocket_send_utf8_text(EMSCRIPTEN_WEBSOCKET_T ws,
                                    const char *text)
{
    FakeSock *s = sock(ws);
    if (s == nullptr || s->sendFails || text == nullptr) {
        return EMSCRIPTEN_RESULT_FAILED;
    }
    s->sentTxt += std::strlen(text);
    return EMSCRIPTEN_RESULT_SUCCESS;
}

extern "C" EMSCRIPTEN_RESULT
emscripten_websocket_send_binary(EMSCRIPTEN_WEBSOCKET_T ws, void *data,
                                 uint32_t size)
{
    FakeSock *s = sock(ws);
    if (s == nullptr || s->sendFails || (data == nullptr && size != 0)) {
        return EMSCRIPTEN_RESULT_FAILED;
    }
    s->sentBin += size;
    return EMSCRIPTEN_RESULT_SUCCESS;
}

extern "C" EMSCRIPTEN_RESULT
emscripten_websocket_close(EMSCRIPTEN_WEBSOCKET_T ws, unsigned short /*code*/,
                           const char * /*reason*/)
{
    FakeSock *s = sock(ws);
    if (s == nullptr) {
        return EMSCRIPTEN_RESULT_FAILED;
    }
    ++sCloseCount;
    return EMSCRIPTEN_RESULT_SUCCESS;
}

extern "C" EMSCRIPTEN_RESULT
emscripten_websocket_delete(EMSCRIPTEN_WEBSOCKET_T ws)
{
    FakeSock *s = sock(ws);
    if (s == nullptr) {
        return EMSCRIPTEN_RESULT_INVALID_PARAM;
    }
    s->deleted = true;
    ++sDeleteCount;
    return EMSCRIPTEN_RESULT_SUCCESS;
}

extern "C" EM_BOOL emscripten_websocket_is_supported(void)
{
    return sSupported ? EM_TRUE : EM_FALSE;
}

extern "C" void emscripten_errf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
}

extern "C" char *emscripten_run_script_string(const char *script)
{
    if (sScriptResult != nullptr) {
        return strdup(sScriptResult);
    }
    const char *search = getenv("FAKE_EMS_LOCATION_SEARCH");
    if (search == nullptr || script == nullptr) {
        return strdup("");
    }
    /* Execute the production JS under a real engine (node). The loader
     * page's location is stubbed to the test-provided query string;
     * everything else — URLSearchParams, the alias table, the
     * newline/'=' filters — is the code that ships in the TU. */
    const char *tmp = "out/probe/fake_ems_run.js";
    FILE *f = fopen(tmp, "w");
    if (f == nullptr) {
        return strdup("");
    }
    fprintf(f, "globalThis.location={search:%s};\n", js_quote(search).c_str());
    fprintf(f, "const r=eval(%s);\n", js_quote(script).c_str());
    fprintf(f, "process.stdout.write(r==null?'':String(r));\n");
    fclose(f);
    const char *node = getenv("FAKE_EMS_NODE");
    if (node == nullptr) {
#if defined(_WIN32)
        node = "node.exe";
#else
        node = "node";
#endif
    }
    std::string cmd = std::string(node) + " " + tmp;
    std::string out;
    if (FILE *pipe = popen(cmd.c_str(), "r")) {
        char buf[512];
        while (fgets(buf, sizeof(buf), pipe) != nullptr) {
            out += buf;
        }
        pclose(pipe);
    }
    return strdup(out.c_str());
}

namespace fake_ems {

void Reset()
{
    sSocks.clear();
    sNow = 0.0;
    sNextId = 1;
    sNewCount = 0;
    sDeleteCount = 0;
    sCloseCount = 0;
    sLast = 0;
    sSupported = true;
    sNextNewFails = false;
    sScriptResult = nullptr;
}

void AdvanceNow(double ms) { sNow += ms; }
double Now() { return sNow; }

int LastSocket() { return sLast; }

void FireOpen(int ws)
{
    FakeSock *s = sock(ws);
    if (s != nullptr && s->onOpen != nullptr) {
        EmscriptenWebSocketOpenEvent ev = {};
        ev.socket = ws;
        s->onOpen(0, &ev, s->userOpen);
    }
}

void FireError(int ws)
{
    FakeSock *s = sock(ws);
    if (s != nullptr && s->onError != nullptr) {
        EmscriptenWebSocketErrorEvent ev = {};
        ev.socket = ws;
        s->onError(0, &ev, s->userError);
    }
}

void FireClose(int ws, unsigned short code, bool wasClean)
{
    FakeSock *s = sock(ws);
    if (s != nullptr && s->onClose != nullptr) {
        EmscriptenWebSocketCloseEvent ev = {};
        ev.socket = ws;
        ev.code = code;
        ev.wasClean = wasClean ? EM_TRUE : EM_FALSE;
        s->onClose(0, &ev, s->userClose);
    }
}

void FireMessageText(int ws, const std::string &payload)
{
    FakeSock *s = sock(ws);
    if (s == nullptr || s->onMsg == nullptr) {
        return;
    }
    EmscriptenWebSocketMessageEvent ev = {};
    ev.socket = ws;
    ev.data = const_cast<uint8_t *>(
        reinterpret_cast<const uint8_t *>(payload.data()));
    ev.numBytes = static_cast<uint32_t>(payload.size());
    ev.isText = EM_TRUE;
    s->onMsg(0, &ev, s->userMsg);
}

void FireMessageBinary(int ws, const void *data, uint32_t size)
{
    FakeSock *s = sock(ws);
    if (s == nullptr || s->onMsg == nullptr) {
        return;
    }
    EmscriptenWebSocketMessageEvent ev = {};
    ev.socket = ws;
    ev.data = const_cast<uint8_t *>(static_cast<const uint8_t *>(data));
    ev.numBytes = size;
    ev.isText = EM_FALSE;
    s->onMsg(0, &ev, s->userMsg);
}

void SetSendFails(int ws, bool fails)
{
    FakeSock *s = sock(ws);
    if (s != nullptr) {
        s->sendFails = fails;
    }
}

void SetSupported(bool supported) { sSupported = supported; }
void SetNextNewFails(bool fails) { sNextNewFails = fails; }
void SetScriptResult(const char *result) { sScriptResult = result; }

int NewCount() { return sNewCount; }
int DeleteCount() { return sDeleteCount; }
int CloseCount() { return sCloseCount; }

size_t SentBinaryBytes(int ws)
{
    FakeSock *s = sock(ws);
    return (s != nullptr) ? s->sentBin : 0;
}

size_t SentTextBytes(int ws)
{
    FakeSock *s = sock(ws);
    return (s != nullptr) ? s->sentTxt : 0;
}

std::string UrlOf(int ws)
{
    FakeSock *s = sock(ws);
    return (s != nullptr) ? s->url : std::string();
}

} // namespace fake_ems
