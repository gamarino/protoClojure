#include "IO.h"

#include "Exceptions.h"
#include "ExecutionEngine.h"
#include "ListBuilder.h"
#include "MapOps.h"
#include "Named.h"
#include "Primitives.h"
#include "VectorOps.h"

#include "protoCore.h"

#include <protoio/error.h>
#include <protoio/file.h>
#include <protoio/http.h>
#include <protoio/net.h>
#include <protoio/process.h>
#include <protoio/stream.h>

#if !defined(_WIN32)
#include <sys/socket.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace protoClojure {

namespace {

using PO = proto::ProtoObject;
using Kind = protoio::Error::Kind;

// ============================================================== handles

enum class HandleKind : int {
    File = 0,
    Reader,
    Writer,
    Socket,
    ServerSocket,
    UdpSocket,
    HttpServer,
};

constexpr const char* kHandleNames[] = {
    "file", "reader", "writer", "socket", "server-socket", "udp-socket", "http-server",
};
constexpr const char* kHandleTypeNames[] = {
    "a file", "a reader", "a writer", "a socket", "a server socket", "a UDP socket",
    "an HTTP server",
};

// Set once per ProtoSpace by installIO, before any other thread runs Clojure
// code. The marker is kept alive as a hidden attribute of the globals.
const PO* g_ioMarker = nullptr;
// Pins the handler of every running HTTP server (one root set per space).
proto::ProtoRootSet* g_ioRoots = nullptr;

const proto::ProtoString* kindKey(proto::ProtoContext* ctx) {
    return proto::ProtoString::createSymbol(ctx, "__io_kind__");
}
const proto::ProtoString* fdKey(proto::ProtoContext* ctx) {
    return proto::ProtoString::createSymbol(ctx, "__io_fd__");
}
const proto::ProtoString* pathKey(proto::ProtoContext* ctx) {
    return proto::ProtoString::createSymbol(ctx, "__io_path__");
}
const proto::ProtoString* serverKey(proto::ProtoContext* ctx) {
    return proto::ProtoString::createSymbol(ctx, "__io_server__");
}

bool isHandle(proto::ProtoContext* ctx, const PO* v) {
    if (!v || v == PROTO_NONE || !g_ioMarker) return false;
    if ((reinterpret_cast<std::uintptr_t>(v) & 0x3F) != 0) return false;  // not an object cell
    return v->getPrototype(ctx) == g_ioMarker;
}

HandleKind handleKind(proto::ProtoContext* ctx, const PO* h) {
    const PO* k = h->getAttribute(ctx, kindKey(ctx));
    return static_cast<HandleKind>(k && k->isInteger(ctx) ? k->asLong(ctx) : 0);
}

std::string handlePath(proto::ProtoContext* ctx, const PO* h) {
    const PO* p = h->getAttribute(ctx, pathKey(ctx));
    if (!p || p == PROTO_NONE || !proto::ProtoObject::isStringTagFast(p)) return "";
    return reinterpret_cast<const proto::ProtoString*>(p)->toStdString(ctx);
}

int handleFd(proto::ProtoContext* ctx, const PO* h) {
    const PO* f = h->getOwnAttributeDirect(ctx, fdKey(ctx));
    return f && f->isInteger(ctx) ? static_cast<int>(f->asLong(ctx)) : -1;
}

// ============================================================== values

bool truthy(const PO* v) { return v && v != PROTO_NONE && v != PROTO_FALSE; }

// A list (protoCore ProtoList, large or small inline form: pointer tags 2
// and 25 of protoCore's headers/proto_internal.h), not a string.
bool isListTagged(const PO* v) {
    if (!v) return false;
    const unsigned t = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(v) & 0x3F);
    return t == 2 || t == 25;
}

bool isString(const PO* v) {
    return v && v != PROTO_NONE && proto::ProtoObject::isStringTagFast(v);
}

std::string bytesOf(proto::ProtoContext* ctx, const PO* s) {
    return reinterpret_cast<const proto::ProtoString*>(s)->toStdString(ctx);
}

// A string holding `bytes` (UTF-8), embedded NULs included. A trailing
// incomplete UTF-8 sequence becomes U+FFFD.
const PO* makeString(proto::ProtoContext* ctx, const std::string& bytes) {
    std::uint8_t rem[4];
    std::uint8_t remCount = 0;
    const proto::ProtoString* s = proto::ProtoString::fromUTF8Buffer(
        ctx, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(),
        nullptr, 0, rem, &remCount);
    if (remCount == 0) return reinterpret_cast<const PO*>(s);
    std::string fixed = bytes.substr(0, bytes.size() - remCount) + "\xEF\xBF\xBD";
    s = proto::ProtoString::fromUTF8Buffer(
        ctx, reinterpret_cast<const std::uint8_t*>(fixed.data()), fixed.size(),
        nullptr, 0, rem, &remCount);
    return reinterpret_cast<const PO*>(s);
}

const ActiveCallContext& callContext() {
    const ActiveCallContext* cc = activeCallContext();
    if (!cc) throw std::logic_error("I/O: no active VM context");
    return *cc;
}

// The keyword spelled `spelling` (":status").
const PO* kw(proto::ProtoContext* ctx, const char* spelling) {
    return internNamed(ctx, callContext().named, spelling);
}

// The spelling of a keyword without its colon ("get" for :get), or of a
// symbol; "" for anything else.
std::string namedName(proto::ProtoContext* ctx, const PO* v) {
    const ActiveCallContext& cc = callContext();
    if (!v || !isNamed(ctx, cc.named, v)) return "";
    std::string s = namedSpelling(ctx, cc.named, v)->toStdString(ctx);
    if (!s.empty() && s[0] == ':') s.erase(0, 1);
    return s;
}

// A map from alternating keys and values. Every element is a young cell of
// `ctx` (or immortal), so it stays alive for the whole build.
const PO* makeMap(proto::ProtoContext* ctx, const std::vector<const PO*>& kv) {
    return mapAssocPairs(ctx, nullptr, kv.data(), kv.size());
}

// What `str` renders for `v`: nil as "", a string as its characters,
// anything else in its printed form.
std::string strOf(proto::ProtoContext* ctx, const PO* v) {
    if (!v || v == PROTO_NONE) return "";
    if (isString(v)) return bytesOf(ctx, v);
    std::string out;
    if (appendIOFilePath(ctx, out, v)) return out;
    printValueTo(ctx, out, v, /*readable=*/true);
    return out;
}

// A list of strings, nil when `items` is empty and `nilWhenEmpty` holds.
const PO* stringList(proto::ProtoContext* ctx, const std::vector<std::string>& items,
                     bool nilWhenEmpty) {
    if (items.empty() && nilWhenEmpty) return PROTO_NONE;
    ListBuilder out(ctx);
    for (const std::string& s : items) out.push(makeString(out.context(), s));
    return out.finish();
}

// ============================================================== errors

[[noreturn]] void arity(proto::ProtoContext* ctx, proto::proto_ulong n, const char* name) {
    throwClassed(ctx, "ArityException",
                 "Wrong number of args (" + std::to_string(n) + ") passed to: " + name);
}

[[noreturn]] void wrongType(proto::ProtoContext* ctx, const char* who, const char* expected,
                            const PO* got) {
    throwClassed(ctx, "ClassCastException",
                 std::string(who) + " expects " + expected + ", got " + valueTypeName(ctx, got));
}

// The class and :type of each protoio::Error kind.
struct KindMapping {
    const char* className;
    const char* type;
};

KindMapping mappingOf(Kind kind) {
    switch (kind) {
        case Kind::FileNotFound:       return {"FileNotFoundException", ":file-not-found"};
        case Kind::FileExists:         return {"IOException", ":file-exists"};
        case Kind::FileSystem:         return {"IOException", ":file-system"};
        case Kind::ConnectionRefused:  return {"ConnectException", ":connection-refused"};
        case Kind::ConnectionTimedOut: return {"SocketTimeoutException", ":timeout"};
        case Kind::NameLookup:         return {"UnknownHostException", ":unknown-host"};
        case Kind::Network:            return {"SocketException", ":network"};
        case Kind::Process:            return {"IOException", ":process"};
        case Kind::LineTooLong:        return {"IOException", ":line-too-long"};
        case Kind::BodyTooLarge:       return {"IOException", ":body-too-large"};
        case Kind::InvalidArgument:    return {"IllegalArgumentException", ":invalid-argument"};
    }
    return {"IOException", ":io"};
}

// Raises the protoClojure exception for a protoio::Error, with the ex-data
// map {:type <kind> :errno <n>} (:errno only when the system reported one).
[[noreturn]] void raise(proto::ProtoContext* ctx, const protoio::Error& e) {
    const KindMapping m = mappingOf(e.kind);
    std::vector<const PO*> kv{kw(ctx, ":type"), kw(ctx, m.type)};
    if (e.sysErrno != 0) {
        kv.push_back(kw(ctx, ":errno"));
        kv.push_back(ctx->fromLong(e.sysErrno));
    }
    throwClassed(ctx, m.className, e.what(), makeMap(ctx, kv));
}

// Runs `f`, which calls protoIO with C++ values only and touches no protoCore
// object, outside the collector's quorum, and answers its result. A
// protoio::Error is raised as a protoClojure exception after the scope has
// been left.
template <typename F>
auto blocking(proto::ProtoContext* ctx, F&& f) -> decltype(f()) {
    try {
        proto::ProtoContext::UnmanagedScope out(ctx);
        return f();
    } catch (const protoio::Error& e) {
        raise(ctx, e);
    }
}

// ============================================================== arguments

proto::proto_ulong argCount(proto::ProtoContext* ctx, const proto::ProtoList* args) {
    return args ? args->getSize(ctx) : 0;
}

const PO* arg(proto::ProtoContext* ctx, const proto::ProtoList* args, proto::proto_ulong i) {
    const PO* v = args->getAt(ctx, static_cast<int>(i));
    return v ? v : PROTO_NONE;
}

std::string stringArg(proto::ProtoContext* ctx, const PO* v, const char* who) {
    if (!isString(v)) wrongType(ctx, who, "a string", v);
    return bytesOf(ctx, v);
}

long long longArg(proto::ProtoContext* ctx, const PO* v, const char* who) {
    if (!v || v == PROTO_NONE || !v->isInteger(ctx)) wrongType(ctx, who, "an integer", v);
    return v->asLong(ctx);
}

int portArg(proto::ProtoContext* ctx, const PO* v, const char* who) {
    const long long p = longArg(ctx, v, who);
    if (p < 0 || p > 65535)
        throwClassed(ctx, "IllegalArgumentException",
                     std::string(who) + ": port out of range: " + std::to_string(p));
    return static_cast<int>(p);
}

// A path: a string or a file handle.
std::string pathArg(proto::ProtoContext* ctx, const PO* v, const char* who) {
    if (isString(v)) return bytesOf(ctx, v);
    if (isHandle(ctx, v) && handleKind(ctx, v) == HandleKind::File) return handlePath(ctx, v);
    wrongType(ctx, who, "a path (a string or a file)", v);
}

// A handle of one of `kinds`, open; its descriptor.
int openFd(proto::ProtoContext* ctx, const PO* v, const char* who,
           std::initializer_list<HandleKind> kinds, HandleKind* kindOut = nullptr) {
    if (!isHandle(ctx, v)) wrongType(ctx, who, "an I/O handle", v);
    const HandleKind k = handleKind(ctx, v);
    if (std::find(kinds.begin(), kinds.end(), k) == kinds.end())
        wrongType(ctx, who, "a suitable I/O handle", v);
    const int fd = handleFd(ctx, v);
    if (fd < 0) throwClassed(ctx, "IOException", "Stream closed");
    if (kindOut) *kindOut = k;
    return fd;
}

// Options after the positional arguments, starting at `from`: one map, or
// keyword/value pairs (`:append true`). Answers a map, or nullptr for none.
const PO* optionsArg(proto::ProtoContext* ctx, const proto::ProtoList* args,
                     proto::proto_ulong from, const char* who) {
    const proto::proto_ulong n = argCount(ctx, args);
    if (from >= n) return nullptr;
    if (n - from == 1) {
        const PO* only = arg(ctx, args, from);
        if (only == PROTO_NONE) return nullptr;
        if (isMap(only)) return only;
    }
    if ((n - from) % 2 != 0)
        throwClassed(ctx, "IllegalArgumentException",
                     std::string(who) + ": options must be a map or keyword/value pairs");
    return mapAssocPairs(ctx, nullptr, args, from, n - from);
}

// The option `spelling` (":append") of `opts`, or nullptr when absent.
const PO* option(proto::ProtoContext* ctx, const PO* opts, const char* spelling) {
    if (!opts) return nullptr;
    bool found = false;
    const PO* v = mapGet(ctx, opts, kw(ctx, spelling), &found);
    return found ? (v ? v : PROTO_NONE) : nullptr;
}

// Rejects any option of `opts` that is not in `known`.
void checkOptions(proto::ProtoContext* ctx, const PO* opts, const char* who,
                  std::initializer_list<const char*> known) {
    if (!opts) return;
    struct Acc {
        std::initializer_list<const char*> known;
        std::string bad;
    } acc{known, ""};
    mapForEach(ctx, opts, &acc, [](proto::ProtoContext* c, void* self, const PO* k, const PO*) {
        auto* a = static_cast<Acc*>(self);
        std::string name = ":" + namedName(c, k);
        if (name == ":") { a->bad = "a non-keyword key"; return; }
        for (const char* ok : a->known)
            if (name == ok) return;
        if (a->bad.empty()) a->bad = name;
    });
    if (!acc.bad.empty())
        throwClassed(ctx, "IllegalArgumentException",
                     std::string(who) + ": unsupported option " + acc.bad);
}

// A timeout option in milliseconds; `dflt` when absent or nil.
int timeoutOption(proto::ProtoContext* ctx, const PO* opts, const char* name, int dflt,
                  const char* who) {
    const PO* v = option(ctx, opts, name);
    if (!v || v == PROTO_NONE) return dflt;
    const long long ms = longArg(ctx, v, who);
    return ms < 0 ? -1 : static_cast<int>(std::min<long long>(ms, 1LL << 30));
}

// A map whose keys are strings, keywords or symbols and whose values are
// strings (or anything `str` renders; a list or vector of values repeats the
// header), as HTTP headers.
protoio::http::Headers headersArg(proto::ProtoContext* ctx, const PO* v, const char* who) {
    protoio::http::Headers out;
    if (!v || v == PROTO_NONE) return out;
    if (!isMap(v)) wrongType(ctx, who, "a map of headers", v);
    mapForEach(ctx, v, &out, [](proto::ProtoContext* c, void* self, const PO* k, const PO* val) {
        auto* hs = static_cast<protoio::http::Headers*>(self);
        std::string name = isString(k) ? bytesOf(c, k) : namedName(c, k);
        if (name.empty()) name = strOf(c, k);
        if (isListTagged(val) || isVector(val)) {
            const proto::ProtoList* items = isVector(val) ? vectorItems(c, val) : val->asList(c);
            const proto::proto_ulong n = items->getSize(c);
            for (proto::proto_ulong i = 0; i < n; ++i)
                hs->push_back({name, strOf(c, items->getAt(c, static_cast<int>(i)))});
        } else {
            hs->push_back({name, strOf(c, val)});
        }
    });
    return out;
}

// ============================================================== handles

const PO* newHandle(proto::ProtoContext* ctx, HandleKind kind, int fd, const std::string& path) {
    if (kind == HandleKind::File) {
        const PO* h = g_ioMarker->newChild(ctx);
        h = h->setAttribute(ctx, kindKey(ctx), ctx->fromLong(static_cast<int>(kind)));
        h = h->setAttribute(ctx, pathKey(ctx), makeString(ctx, path));
        return h;
    }
    const PO* h = g_ioMarker->newChild(ctx, /*isMutable=*/true);
    h->setAttribute(ctx, kindKey(ctx), ctx->fromLong(static_cast<int>(kind)));
    h->setAttribute(ctx, pathKey(ctx), makeString(ctx, path));
    h->setAttribute(ctx, fdKey(ctx), ctx->fromLong(fd));
    return h;
}

// Takes the descriptor out of `h` (a compare-and-set to -1, so exactly one
// caller wins) and answers it, or -1 when it was closed already.
int takeFd(proto::ProtoContext* ctx, const PO* h) {
    for (;;) {
        const PO* cur = h->getOwnAttributeDirect(ctx, fdKey(ctx));
        if (!cur || !cur->isInteger(ctx) || cur->asLong(ctx) < 0) return -1;
        if (h->setAttributeIfEqual(ctx, fdKey(ctx), cur, ctx->fromLong(-1)))
            return static_cast<int>(cur->asLong(ctx));
    }
}

// ============================================================== the HTTP server

struct Connection {
    std::mutex mutex;          // guards `closed` and `reading`
    int fd = -1;
    bool closed = false;       // the connection thread closed `fd`
    bool reading = true;       // waiting for the request (safe to wake)
    std::atomic<bool> done{false};
    const proto::ProtoThread* thread = nullptr;
};

struct HttpServer {
    int listenFd = -1;
    int port = 0;
    std::string host;
    ActiveCallContext cc{};                      // installed on every server thread
    const PO* handler = nullptr;                 // pinned in g_ioRoots
    proto::ProtoRootSet::Handle pin = proto::ProtoRootSet::kNullHandle;
    protoio::http::Limits limits;
    int readTimeoutMs = 30000;
    const proto::ProtoThread* acceptThread = nullptr;
    std::atomic<bool> stopping{false};
    std::atomic<bool> joined{false};
#if defined(_WIN32) || defined(__APPLE__)
    std::atomic<bool> inAccept{false};           // the accept thread is in tcpAccept
#endif
    std::mutex mutex;                            // guards the fields below
    bool listenClosed = false;
    std::vector<std::shared_ptr<Connection>> connections;
};

#if defined(__APPLE__)
// How long the accept thread waits in one tcpAccept call before it looks at
// `stopping` again (see requestStop).
constexpr int kAcceptSliceMs = 50;
#endif

std::mutex g_serversMutex;
std::vector<std::unique_ptr<HttpServer>> g_servers;  // never shrinks: states live for the process
thread_local HttpServer* t_servingFor = nullptr;    // the server whose handler runs here

// Stops accepting and wakes every connection still waiting for its request;
// requests being handled finish. The accept thread and the connection
// threads are the only ones that ever close their descriptors, so a
// descriptor number is never released under a thread still using it: this
// only shuts the sockets down, which wakes a blocked accept or read.
void requestStop(HttpServer* s) {
    if (s->stopping.exchange(true)) return;
    std::unique_lock<std::mutex> lock(s->mutex);
#if defined(_WIN32)
    // Windows: a shutdown wakes neither a blocked accept nor a blocked read.
    // protoIO waits there in short WSAPoll slices and watches the
    // descriptor's closed flag, which only protoio::close sets, so the
    // sockets are closed here instead, and marked so that their own threads
    // leave them alone. This keeps the rule above: the state a waiting
    // protoIO call holds keeps its socket open until the call returns, and
    // protoIO numbers Windows sockets itself and does not reuse a number
    // until it wraps around, so a later use of the old number fails cleanly.
    if (!s->listenClosed) {
        s->listenClosed = true;
        protoio::close(s->listenFd);
    }
    for (const auto& c : s->connections) {
        std::lock_guard<std::mutex> cl(c->mutex);
        if (!c->closed && c->reading) {
            c->closed = true;
            protoio::close(c->fd);
        }
    }
    lock.unlock();
    // The listening socket is released when the waiting tcpAccept returns,
    // within one slice. Until then Windows still queues new connections on
    // it, and resets them when it goes; on Linux they are refused at once.
    // Wait for it, so that a client is refused once stop-server returned.
    while (s->inAccept.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
#elif defined(__APPLE__)
    // macOS: a shutdown does not wake a poll on a listening socket, nor make
    // it refuse connections (it answers ENOTCONN), so the listener must be
    // closed. Not while the accept thread may be entering or inside
    // tcpAccept: protoio::close releases the descriptor number as soon as no
    // protoIO call holds it, and a call about to start could then use a
    // number reused by someone else. So the listener is closed here only when
    // the accept thread is outside tcpAccept (s->mutex orders this against
    // its entry); otherwise the accept thread, which waits in short slices
    // (kAcceptSliceMs), closes it as it leaves the call, and this waits for
    // that, so that a client is refused once stop-server returned. A shutdown
    // does wake a read on a connected socket, as on Linux.
    if (!s->listenClosed && !s->inAccept.load()) {
        s->listenClosed = true;
        protoio::close(s->listenFd);
    }
    for (const auto& c : s->connections) {
        std::lock_guard<std::mutex> cl(c->mutex);
        if (!c->closed && c->reading) ::shutdown(c->fd, SHUT_RDWR);
    }
    lock.unlock();
    while (s->inAccept.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
#else
    if (!s->listenClosed) ::shutdown(s->listenFd, SHUT_RDWR);
    for (const auto& c : s->connections) {
        std::lock_guard<std::mutex> cl(c->mutex);
        if (!c->closed && c->reading) ::shutdown(c->fd, SHUT_RDWR);
    }
#endif
}

// Joins the connection threads that finished (all of them with `all`).
void joinConnections(proto::ProtoContext* ctx, HttpServer* s, bool all) {
    std::vector<std::shared_ptr<Connection>> finished;
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        auto& cs = s->connections;
        for (auto it = cs.begin(); it != cs.end();) {
            if (all || (*it)->done.load()) {
                finished.push_back(*it);
                it = cs.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& c : finished)
        if (c->thread) const_cast<proto::ProtoThread*>(c->thread)->join(ctx);
}

void closeConnection(Connection* c) {
    std::lock_guard<std::mutex> lock(c->mutex);
    if (c->closed) return;
    c->closed = true;
    protoio::close(c->fd);
}

// The host a listening socket binds when none was named: every interface.
// protoIO resolves the empty host to the wildcard addresses and binds the
// first. Linux lists 0.0.0.0 first; Windows lists :: first, and a Windows
// IPv6 socket is IPv6-only by default, so a server there would not answer
// 127.0.0.1. Naming 0.0.0.0 on Windows binds what Linux binds.
std::string listenHost(const std::string& host) {
#if defined(_WIN32)
    if (host.empty()) return "0.0.0.0";
#endif
    return host;
}

void report(const std::string& text) {
    std::fprintf(stderr, "protoclj: %s\n", text.c_str());
    std::fflush(stderr);
}

// A plain-text response (refusals and handler failures). Errors writing it
// are ignored: the peer may be gone.
void writePlain(int fd, int status) {
    try {
        protoio::http::ResponseHead head;
        head.status = status;
        head.headers.push_back({"content-type", "text/plain; charset=utf-8"});
        protoio::http::writeResponse(fd, head, protoio::http::reasonPhrase(status));
    } catch (const std::exception&) {
    }
}

// What the handler answered, as C++ values.
struct Answer {
    int status = 200;
    protoio::http::Headers headers;
    std::string body;
    std::string bodyFile;  // a file handle body: read before writing
    bool fromFile = false;
    std::string error;     // not empty: the answer was unusable
};

Answer answerOf(proto::ProtoContext* ctx, const PO* r) {
    Answer a;
    if (!isMap(r)) {
        a.error = std::string("run-server: the handler answered ") + valueTypeName(ctx, r) +
                  ", not a response map";
        return a;
    }
    if (const PO* st = option(ctx, r, ":status")) {
        if (st != PROTO_NONE) {
            if (!st->isInteger(ctx) || st->asLong(ctx) < 100 || st->asLong(ctx) > 999) {
                a.error = "run-server: :status must be an integer from 100 to 999";
                return a;
            }
            a.status = static_cast<int>(st->asLong(ctx));
        }
    }
    if (const PO* hs = option(ctx, r, ":headers")) {
        if (hs != PROTO_NONE && !isMap(hs)) {
            a.error = "run-server: :headers must be a map";
            return a;
        }
        a.headers = headersArg(ctx, hs, "run-server");
    }
    if (const PO* b = option(ctx, r, ":body")) {
        if (isHandle(ctx, b) && handleKind(ctx, b) == HandleKind::File) {
            a.fromFile = true;
            a.bodyFile = handlePath(ctx, b);
        } else if (isListTagged(b) || isVector(b)) {
            const proto::ProtoList* items = isVector(b) ? vectorItems(ctx, b) : b->asList(ctx);
            const proto::proto_ulong n = items->getSize(ctx);
            for (proto::proto_ulong i = 0; i < n; ++i)
                a.body += strOf(ctx, items->getAt(ctx, static_cast<int>(i)));
        } else {
            a.body = strOf(ctx, b);
        }
    }
    return a;
}

// The Ring request map for a request.
const PO* requestMap(proto::ProtoContext* ctx, HttpServer* s,
                     const protoio::http::RequestHead& head, const std::string& body,
                     const std::string& remote) {
    std::string uri = head.target;
    std::optional<std::string> query;
    const std::size_t q = uri.find('?');
    if (q != std::string::npos) {
        query = uri.substr(q + 1);
        uri.erase(q);
    }
    // Header names are lower case (protoIO); repeated headers are joined with
    // ",", as Ring does.
    std::vector<std::pair<std::string, std::string>> merged;
    for (const auto& h : head.headers) {
        auto it = std::find_if(merged.begin(), merged.end(),
                               [&](const auto& m) { return m.first == h.name; });
        if (it == merged.end()) merged.emplace_back(h.name, h.value);
        else it->second += "," + h.value;
    }
    std::vector<const PO*> hkv;
    for (const auto& [name, value] : merged) {
        hkv.push_back(makeString(ctx, name));
        hkv.push_back(makeString(ctx, value));
    }
    std::string serverName = "localhost";
    if (auto host = protoio::http::find(head.headers, "host")) {
        serverName = *host;
        if (!serverName.empty() && serverName[0] == '[') {
            const std::size_t close = serverName.find(']');
            if (close != std::string::npos) serverName = serverName.substr(1, close - 1);
        } else if (const std::size_t colon = serverName.rfind(':'); colon != std::string::npos) {
            serverName.erase(colon);
        }
    }
    std::string method = head.method;
    for (char& c : method) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string methodKw = ":" + method;
    std::vector<const PO*> kv{
        kw(ctx, ":server-port"),    ctx->fromLong(s->port),
        kw(ctx, ":server-name"),    makeString(ctx, serverName),
        kw(ctx, ":remote-addr"),    makeString(ctx, remote),
        kw(ctx, ":uri"),            makeString(ctx, uri),
        kw(ctx, ":query-string"),   query ? makeString(ctx, *query) : PROTO_NONE,
        kw(ctx, ":scheme"),         kw(ctx, ":http"),
        kw(ctx, ":request-method"), kw(ctx, methodKw.c_str()),
        kw(ctx, ":protocol"),       makeString(ctx, head.version.empty() ? "HTTP/1.0" : head.version),
        kw(ctx, ":headers"),        makeMap(ctx, hkv),
        kw(ctx, ":body"),           body.empty() ? PROTO_NONE : makeString(ctx, body),
    };
    return makeMap(ctx, kv);
}

// One connection: read the request (refusing malformed or oversized input
// with 400/414/431/413 before the handler runs), call the handler, validate
// and write its answer (a response protoIO refuses, such as a header value
// with a line break, becomes a plain 500), close.
void serveConnection(proto::ProtoContext* ctx, HttpServer* s, Connection* c) {
    const int fd = c->fd;
    std::optional<protoio::http::RequestHead> head;
    std::string body;
    std::string remote;
    int refusal = 0;
    {
        proto::ProtoContext::UnmanagedScope out(ctx);
        try {
            protoio::setTimeout(fd, s->readTimeoutMs);
            try { remote = protoio::net::peerName(fd).host; } catch (const protoio::Error&) {}
            head = protoio::http::readRequestHead(fd, s->limits);
            if (head) body = protoio::http::readBody(fd, head->headers, s->limits.maxBody);
        } catch (const protoio::http::HttpRefusal& e) {
            refusal = e.status;
        } catch (const protoio::Error& e) {
            // A body over the limit, or one that is malformed or ends early;
            // anything else (a timeout, a reset, a stop) just ends the
            // connection.
            if (e.kind == Kind::BodyTooLarge) refusal = 413;
            else if (e.kind == Kind::Network && head) refusal = 400;
            else head.reset();
        }
    }
    {
        std::lock_guard<std::mutex> lock(c->mutex);
        c->reading = false;
    }
    if (refusal != 0 || !head) {
        if (refusal != 0) {
            proto::ProtoContext::UnmanagedScope out(ctx);
            writePlain(fd, refusal);
        }
        closeConnection(c);
        return;
    }

    Answer answer;
    {
        // Everything the request builds is this context's young generation,
        // handed to the collector when it is destroyed.
        proto::ProtoContext req(ctx->space, ctx);
        req.resizeAutomaticLocals(2);
        try {
            req.setAutomaticLocal(0, requestMap(&req, s, *head, body, remote));
            const PO* argv[1] = {req.getAutomaticLocal(0)};
            req.setAutomaticLocal(1, s->cc.engine->invoke(&req, s->handler, argv, 1));
            answer = answerOf(&req, req.getAutomaticLocal(1));
        } catch (const std::exception& e) {
            answer.error = std::string("run-server: the handler threw ") + e.what();
        }
    }
    if (!answer.error.empty()) report(answer.error);
    {
        proto::ProtoContext::UnmanagedScope out(ctx);
        if (!answer.error.empty()) {
            writePlain(fd, 500);
        } else {
            try {
                if (answer.fromFile) answer.body = protoio::file::read(answer.bodyFile);
                protoio::http::ResponseHead rh;
                rh.status = answer.status;
                rh.headers = std::move(answer.headers);
                protoio::http::writeResponse(fd, rh, answer.body);
            } catch (const protoio::Error& e) {
                if (e.kind == Kind::InvalidArgument || e.kind == Kind::FileNotFound ||
                    e.kind == Kind::FileSystem) {
                    report(std::string("run-server: the response was refused: ") + e.what());
                    writePlain(fd, 500);
                }
                // Otherwise the peer went away: nothing to answer.
            }
        }
    }
    closeConnection(c);
}

// The main of a connection thread: args are the server and the connection,
// each a pointer carried as an integer.
const PO* connectionMain(proto::ProtoContext* ctx, const PO*, const proto::ParentLink*,
                         const proto::ProtoList* args, const proto::ProtoSparseList*) {
    auto* s = reinterpret_cast<HttpServer*>(args->getAt(ctx, 0)->asLong(ctx));
    auto* c = reinterpret_cast<Connection*>(args->getAt(ctx, 1)->asLong(ctx));
    setActiveCallContext(s->cc);
    t_servingFor = s;
    try {
        serveConnection(ctx, s, c);
    } catch (const std::exception& e) {
        report(std::string("run-server: ") + e.what());
        closeConnection(c);
    }
    t_servingFor = nullptr;
    clearActiveCallContext();
    c->done.store(true);
    return PROTO_NONE;
}

// The main of a server's accept thread: accepts until the server stops, one
// connection thread per connection, then joins them all and closes the
// listening socket.
const PO* acceptMain(proto::ProtoContext* ctx, const PO*, const proto::ParentLink*,
                     const proto::ProtoList* args, const proto::ProtoSparseList*) {
    auto* s = reinterpret_cast<HttpServer*>(args->getAt(ctx, 0)->asLong(ctx));
    setActiveCallContext(s->cc);
    while (!s->stopping.load()) {
        joinConnections(ctx, s, /*all=*/false);
        std::optional<int> accepted;
        try {
            proto::ProtoContext::UnmanagedScope out(ctx);
#if defined(_WIN32)
            // Cleared before the scope ends: requestStop waits for it without
            // reaching a safepoint.
            struct InAccept {
                std::atomic<bool>& flag;
                explicit InAccept(std::atomic<bool>& f) : flag(f) { flag.store(true); }
                ~InAccept() { flag.store(false); }
            } inAccept(s->inAccept);
            if (s->stopping.load()) break;
            accepted = protoio::net::tcpAccept(s->listenFd, -1);
#elif defined(__APPLE__)
            // See requestStop. Entering and leaving tcpAccept are ordered
            // against it by s->mutex; whoever sees the other side last
            // closes the listener. Cleared before the scope ends, as on
            // Windows.
            {
                std::lock_guard<std::mutex> lock(s->mutex);
                if (s->stopping.load()) break;
                s->inAccept.store(true);
            }
            struct InAccept {
                HttpServer* s;
                ~InAccept() {
                    std::lock_guard<std::mutex> lock(s->mutex);
                    if (s->stopping.load() && !s->listenClosed) {
                        s->listenClosed = true;
                        protoio::close(s->listenFd);
                    }
                    s->inAccept.store(false);
                }
            } inAccept{s};
            accepted = protoio::net::tcpAccept(s->listenFd, kAcceptSliceMs);
#else
            accepted = protoio::net::tcpAccept(s->listenFd, -1);
#endif
        } catch (const protoio::Error& e) {
            if (s->stopping.load()) break;
            report(std::string("run-server: accept failed: ") + e.what());
            continue;
        }
        if (!accepted) continue;
        auto conn = std::make_shared<Connection>();
        conn->fd = *accepted;
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            s->connections.push_back(conn);
        }
        if (s->stopping.load()) {
            closeConnection(conn.get());
            conn->done.store(true);
            break;
        }
        // The argument list is rooted by the new thread; the child context
        // hands this iteration's cells to the collector.
        proto::ProtoContext spawn(ctx->space, ctx);
        const proto::ProtoList* targs = spawn.newList()
            ->appendLast(&spawn, spawn.fromLong(reinterpret_cast<long long>(s)))
            ->appendLast(&spawn, spawn.fromLong(reinterpret_cast<long long>(conn.get())));
        conn->thread = ctx->space->newThread(
            &spawn, proto::ProtoString::createSymbol(&spawn, "protoclj-http-connection"),
            &connectionMain, targs, nullptr);
    }
    joinConnections(ctx, s, /*all=*/true);
    {
        std::lock_guard<std::mutex> lock(s->mutex);
        if (!s->listenClosed) {
            s->listenClosed = true;
            protoio::close(s->listenFd);
        }
    }
    clearActiveCallContext();
    return PROTO_NONE;
}

// Stops `s` and, unless called from one of its own handlers, waits for its
// threads and unpins its handler. A handler that stops its own server returns
// at once; its threads are joined by shutdownIO.
void stopServer(proto::ProtoContext* ctx, HttpServer* s) {
    requestStop(s);
    if (t_servingFor == s) return;
    if (s->joined.exchange(true)) return;
    if (s->acceptThread) {
        proto::ProtoContext::UnmanagedScope out(ctx);
        const_cast<proto::ProtoThread*>(s->acceptThread)->join(ctx);
    }
    if (s->pin != proto::ProtoRootSet::kNullHandle) {
        g_ioRoots->remove(s->pin);
        s->pin = proto::ProtoRootSet::kNullHandle;
    }
}

HttpServer* serverOf(proto::ProtoContext* ctx, const PO* h, const char* who) {
    if (!isHandle(ctx, h) || handleKind(ctx, h) != HandleKind::HttpServer)
        wrongType(ctx, who, "an HTTP server", h);
    const PO* p = h->getAttribute(ctx, serverKey(ctx));
    return reinterpret_cast<HttpServer*>(p->asLong(ctx));
}

// ============================================================== primitives

#define IOPRIM(NAME)                                                                 \
    const PO* NAME(proto::ProtoContext* ctx, const PO*, const proto::ParentLink*,  \
                   const proto::ProtoList* args, const proto::ProtoSparseList*)

bool isHttpUrl(const std::string& s) {
    return s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0;
}

// The encoding option of slurp / spit: only UTF-8 is supported.
void checkEncoding(proto::ProtoContext* ctx, const PO* opts, const char* who) {
    checkOptions(ctx, opts, who, {":encoding", ":append"});
    const PO* enc = option(ctx, opts, ":encoding");
    if (!enc || enc == PROTO_NONE) return;
    std::string e = stringArg(ctx, enc, who);
    for (char& ch : e) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (e != "utf-8" && e != "utf8")
        throwClassed(ctx, "IllegalArgumentException",
                     std::string(who) + ": unsupported encoding " + e + " (only UTF-8)");
}

// (slurp f & opts): the whole contents of a file, a reader or an http(s) URL.
IOPRIM(prim_slurp) {
    const proto::proto_ulong n = argCount(ctx, args);
    if (n < 1) arity(ctx, n, "slurp");
    const PO* src = arg(ctx, args, 0);
    checkEncoding(ctx, optionsArg(ctx, args, 1, "slurp"), "slurp");
    if (isHandle(ctx, src) && handleKind(ctx, src) != HandleKind::File) {
        const int fd = openFd(ctx, src, "slurp", {HandleKind::Reader, HandleKind::Socket});
        const std::string all = blocking(ctx, [&] { return protoio::readAll(fd); });
        return makeString(ctx, all);
    }
    const std::string path = pathArg(ctx, src, "slurp");
    if (isString(src) && isHttpUrl(path)) {
        protoio::http::Request rq;
        rq.url = path;
        const protoio::http::Response r = blocking(ctx, [&] { return protoio::http::httpRequest(rq); });
        if (r.status >= 400) {
            // As java.net.URL's stream does for an error status.
            if (r.status == 404 || r.status == 410)
                throwClassed(ctx, "FileNotFoundException", path);
            throwClassed(ctx, "IOException", "Server returned HTTP response code: " +
                                                 std::to_string(r.status) + " for URL: " + path);
        }
        return makeString(ctx, r.body);
    }
    const std::string all = blocking(ctx, [&] { return protoio::file::read(path); });
    return makeString(ctx, all);
}

// (spit f content & {:keys [append]}): writes (str content) to a file or a
// writer; nil.
IOPRIM(prim_spit) {
    const proto::proto_ulong n = argCount(ctx, args);
    if (n < 2) arity(ctx, n, "spit");
    const PO* dst = arg(ctx, args, 0);
    const PO* opts = optionsArg(ctx, args, 2, "spit");
    checkEncoding(ctx, opts, "spit");
    const bool append = truthy(option(ctx, opts, ":append"));
    const std::string content = strOf(ctx, arg(ctx, args, 1));
    if (isHandle(ctx, dst) && handleKind(ctx, dst) != HandleKind::File) {
        const int fd = openFd(ctx, dst, "spit", {HandleKind::Writer, HandleKind::Socket});
        blocking(ctx, [&] { protoio::write(fd, content); });
        return PROTO_NONE;
    }
    const std::string path = pathArg(ctx, dst, "spit");
    blocking(ctx, [&] {
        if (append) protoio::file::append(path, content);
        else protoio::file::write(path, content);
    });
    return PROTO_NONE;
}

// (read-line) from standard input, or (read-line rdr) from a reader or a
// socket: the next line without its end, nil at the end of the stream.
IOPRIM(prim_read_line) {
    const proto::proto_ulong n = argCount(ctx, args);
    if (n > 1) arity(ctx, n, "read-line");
    const int fd = n == 0 ? 0
        : openFd(ctx, arg(ctx, args, 0), "read-line", {HandleKind::Reader, HandleKind::Socket});
    const std::optional<std::string> line = blocking(ctx, [&] { return protoio::readLine(fd); });
    return line ? makeString(ctx, *line) : PROTO_NONE;
}

// (write w x): writes (str x) to a writer or a socket; nil.
IOPRIM(prim_write) {
    const proto::proto_ulong n = argCount(ctx, args);
    if (n != 2) arity(ctx, n, "write");
    const int fd = openFd(ctx, arg(ctx, args, 0), "write", {HandleKind::Writer, HandleKind::Socket});
    const std::string data = strOf(ctx, arg(ctx, args, 1));
    blocking(ctx, [&] { protoio::write(fd, data); });
    return PROTO_NONE;
}

// (close h): closes a reader, writer, socket or listening socket, or stops an
// HTTP server; closing twice is harmless. nil.
IOPRIM(prim_close) {
    const proto::proto_ulong n = argCount(ctx, args);
    if (n != 1) arity(ctx, n, "close");
    const PO* h = arg(ctx, args, 0);
    if (h == PROTO_NONE) return PROTO_NONE;  // (with-open [x nil] ...) as in Clojure
    if (!isHandle(ctx, h)) wrongType(ctx, "close", "an I/O handle", h);
    const HandleKind k = handleKind(ctx, h);
    if (k == HandleKind::File) return PROTO_NONE;
    if (k == HandleKind::HttpServer) {
        stopServer(ctx, serverOf(ctx, h, "close"));
        return PROTO_NONE;
    }
    const int fd = takeFd(ctx, h);
    if (fd >= 0) blocking(ctx, [&] { protoio::close(fd); });
    return PROTO_NONE;
}

// (reader f): a reader on a file.
IOPRIM(prim_reader) {
    const proto::proto_ulong n = argCount(ctx, args);
    if (n < 1) arity(ctx, n, "reader");
    const PO* src = arg(ctx, args, 0);
    if (isHandle(ctx, src) && handleKind(ctx, src) == HandleKind::Reader) return src;
    checkEncoding(ctx, optionsArg(ctx, args, 1, "reader"), "reader");
    const std::string path = pathArg(ctx, src, "reader");
    const int fd = blocking(ctx, [&] { return protoio::file::open(path, protoio::file::Mode::Read); });
    return newHandle(ctx, HandleKind::Reader, fd, path);
}

// (writer f & {:keys [append]}): a writer on a file, truncated unless
// :append is true.
IOPRIM(prim_writer) {
    const proto::proto_ulong n = argCount(ctx, args);
    if (n < 1) arity(ctx, n, "writer");
    const PO* dst = arg(ctx, args, 0);
    if (isHandle(ctx, dst) && handleKind(ctx, dst) == HandleKind::Writer) return dst;
    const PO* opts = optionsArg(ctx, args, 1, "writer");
    checkEncoding(ctx, opts, "writer");
    const bool append = truthy(option(ctx, opts, ":append"));
    const std::string path = pathArg(ctx, dst, "writer");
    const int fd = blocking(ctx, [&] {
        return protoio::file::open(path, append ? protoio::file::Mode::Append
                                                : protoio::file::Mode::Write);
    });
    return newHandle(ctx, HandleKind::Writer, fd, path);
}

// (line-seq rdr): the remaining lines of a reader or a socket, read to the
// end (eager: deviation D29); nil when there are none.
IOPRIM(prim_line_seq) {
    const proto::proto_ulong n = argCount(ctx, args);
    if (n != 1) arity(ctx, n, "line-seq");
    const int fd = openFd(ctx, arg(ctx, args, 0), "line-seq", {HandleKind::Reader, HandleKind::Socket});
    const std::vector<std::string> lines = blocking(ctx, [&] {
        std::vector<std::string> out;
        while (auto line = protoio::readLine(fd)) out.push_back(std::move(*line));
        return out;
    });
    return stringList(ctx, lines, /*nilWhenEmpty=*/true);
}

// (file path & more): a file handle naming `path`, each further argument a
// child of the one before.
IOPRIM(prim_file) {
    const proto::proto_ulong n = argCount(ctx, args);
    if (n < 1) arity(ctx, n, "file");
    std::string path = pathArg(ctx, arg(ctx, args, 0), "file");
    for (proto::proto_ulong i = 1; i < n; ++i) {
        const std::string child = pathArg(ctx, arg(ctx, args, i), "file");
        if (!path.empty() && path.back() != '/') path += '/';
        path += child;
    }
    return newHandle(ctx, HandleKind::File, -1, path);
}

// (file-seq dir): the file itself and, for a directory, every file below it,
// depth first, each directory's entries in name order; file handles.
IOPRIM(prim_file_seq) {
    const proto::proto_ulong n = argCount(ctx, args);
    if (n != 1) arity(ctx, n, "file-seq");
    const std::string root = pathArg(ctx, arg(ctx, args, 0), "file-seq");
    const std::vector<std::string> paths = blocking(ctx, [&] {
        std::vector<std::string> out;
        struct Walk {
            std::vector<std::string>& out;
            void operator()(const std::string& p) {
                out.push_back(p);
                const auto st = protoio::file::stat(p);
                if (!st || !st->isDirectory) return;
                for (const std::string& name : protoio::file::list(p))
                    (*this)((p.empty() || p.back() == '/') ? p + name : p + "/" + name);
            }
        } walk{out};
        walk(root);
        return out;
    });
    ListBuilder out(ctx);
    for (const std::string& p : paths) out.push(newHandle(out.context(), HandleKind::File, -1, p));
    return out.finish();
}

// (exists? f), (directory? f): whether something, or a directory, is there.
IOPRIM(prim_exists_p) {
    const proto::proto_ulong n = argCount(ctx, args);
    if (n != 1) arity(ctx, n, "exists?");
    const std::string path = pathArg(ctx, arg(ctx, args, 0), "exists?");
    const bool there = blocking(ctx, [&] { return protoio::file::stat(path).has_value(); });
    return there ? PROTO_TRUE : PROTO_FALSE;
}

IOPRIM(prim_directory_p) {
    const proto::proto_ulong n = argCount(ctx, args);
    if (n != 1) arity(ctx, n, "directory?");
    const std::string path = pathArg(ctx, arg(ctx, args, 0), "directory?");
    const bool dir = blocking(ctx, [&] {
        const auto st = protoio::file::stat(path);
        return st && st->isDirectory;
    });
    return dir ? PROTO_TRUE : PROTO_FALSE;
}

// (delete-file f & [silently]): deletes a file or an empty directory; true.
// A failure throws IOException "Couldn't delete f", unless `silently` is
// truthy, in which case it answers `silently` (clojure.java.io/delete-file).
IOPRIM(prim_delete_file) {
    const proto::proto_ulong n = argCount(ctx, args);
    if (n < 1 || n > 2) arity(ctx, n, "delete-file");
    const std::string path = pathArg(ctx, arg(ctx, args, 0), "delete-file");
    const PO* silently = n == 2 ? arg(ctx, args, 1) : PROTO_FALSE;
    bool removed = false;
    {
        proto::ProtoContext::UnmanagedScope out(ctx);
        try { removed = protoio::file::remove(path, false); } catch (const protoio::Error&) {}
    }
    if (removed) return PROTO_TRUE;
    if (truthy(silently)) return silently;
    throwClassed(ctx, "IOException", "Couldn't delete " + path);
}

// (make-parents f & more): creates the missing parent directories of the
// file (clojure.java.io/make-parents); true when it created any.
IOPRIM(prim_make_parents) {
    const proto::proto_ulong n = argCount(ctx, args);
    if (n < 1) arity(ctx, n, "make-parents");
    std::string path = pathArg(ctx, arg(ctx, args, 0), "make-parents");
    for (proto::proto_ulong i = 1; i < n; ++i) {
        if (!path.empty() && path.back() != '/') path += '/';
        path += pathArg(ctx, arg(ctx, args, i), "make-parents");
    }
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0) return PROTO_FALSE;
    const std::string parent = path.substr(0, slash);
    const bool created = blocking(ctx, [&] {
        if (protoio::file::stat(parent)) return false;
        protoio::file::mkdir(parent, /*parents=*/true);
        return true;
    });
    return created ? PROTO_TRUE : PROTO_FALSE;
}

// (copy input output): copies a string's characters, a file's or a reader's
// contents into a file or a writer (clojure.java.io/copy); a file into a
// file copies whole trees too. nil.
IOPRIM(prim_copy) {
    const proto::proto_ulong n = argCount(ctx, args);
    if (n < 2) arity(ctx, n, "copy");
    const PO* in = arg(ctx, args, 0);
    const PO* outv = arg(ctx, args, 1);
    checkOptions(ctx, optionsArg(ctx, args, 2, "copy"), "copy", {":encoding", ":buffer-size"});
    HandleKind outKind = HandleKind::File;
    int outFd = -1;
    std::string outPath;
    if (isHandle(ctx, outv) && handleKind(ctx, outv) == HandleKind::File) {
        outPath = handlePath(ctx, outv);
    } else if (isHandle(ctx, outv)) {
        outFd = openFd(ctx, outv, "copy", {HandleKind::Writer, HandleKind::Socket}, &outKind);
    } else {
        throwClassed(ctx, "IllegalArgumentException",
                     std::string("copy: the output must be a file or a writer, got ") +
                         valueTypeName(ctx, outv));
    }
    std::optional<std::string> content;
    std::string inPath;
    int inFd = -1;
    if (isString(in)) {
        content = bytesOf(ctx, in);
    } else if (isHandle(ctx, in) && handleKind(ctx, in) == HandleKind::File) {
        inPath = handlePath(ctx, in);
    } else if (isHandle(ctx, in)) {
        inFd = openFd(ctx, in, "copy", {HandleKind::Reader, HandleKind::Socket});
    } else {
        throwClassed(ctx, "IllegalArgumentException",
                     std::string("copy: the input must be a string, a file or a reader, got ") +
                         valueTypeName(ctx, in));
    }
    blocking(ctx, [&] {
        if (!content && !inPath.empty() && outFd < 0) {
            protoio::file::copy(inPath, outPath);
            return;
        }
        std::string data = content ? *content
                         : !inPath.empty() ? protoio::file::read(inPath)
                                           : protoio::readAll(inFd);
        if (outFd >= 0) protoio::write(outFd, data);
        else protoio::file::write(outPath, data);
    });
    return PROTO_NONE;
}

// ---------------------------------------------------------------- shell

// Searches `cmd` in PATH as posix_spawnp would; true when it names an
// executable file.
bool commandExists(const std::string& cmd, std::string* found = nullptr) {
    auto executable = [](const std::string& p) {
        const auto st = protoio::file::stat(p);
        return st && st->isFile;
    };
#if defined(_WIN32)
    // Windows: PATH is ';'-separated, either slash separates directories, and
    // a program may be named without its ".exe", which CreateProcess (and so
    // protoIO's run) appends.
    auto program = [&](const std::string& p) {
        for (const std::string& name : {p, p + ".exe"}) {
            if (!executable(name)) continue;
            if (found) *found = name;
            return true;
        }
        return false;
    };
    if (cmd.find_first_of("/\\") != std::string::npos) return program(cmd);
    const auto path = protoio::process::getenv("PATH");
    const std::string dirs = path ? *path : "";
    constexpr char kPathListSeparator = ';';
#else
    (void)found;  // the found path is only needed on Windows
    auto program = executable;
    if (cmd.find('/') != std::string::npos) return executable(cmd);
    const auto path = protoio::process::getenv("PATH");
    const std::string dirs = path ? *path : "/usr/local/bin:/usr/bin:/bin";
    constexpr char kPathListSeparator = ':';
#endif
    std::size_t start = 0;
    for (;;) {
        const std::size_t colon = dirs.find(kPathListSeparator, start);
        std::string dir = dirs.substr(start, colon == std::string::npos ? std::string::npos
                                                                        : colon - start);
        if (dir.empty()) dir = ".";
        if (program(dir + "/" + cmd)) return true;
        if (colon == std::string::npos) return false;
        start = colon + 1;
    }
}

// (sh cmd & args-and-options): runs a program and answers
// {:exit n :out "..." :err "..."} (clojure.java.shell/sh). Options: :in (a
// string fed to its standard input), :dir (its working directory), :env (a
// map replacing its environment).
IOPRIM(prim_sh) {
    const proto::proto_ulong n = argCount(ctx, args);
    std::vector<std::string> argv;
    proto::proto_ulong i = 0;
    for (; i < n; ++i) {
        const PO* a = arg(ctx, args, i);
        if (!isString(a)) break;
        argv.push_back(bytesOf(ctx, a));
    }
    if (argv.empty())
        throwClassed(ctx, "IllegalArgumentException", "sh: needs a command");
    const PO* opts = optionsArg(ctx, args, i, "sh");
    checkOptions(ctx, opts, "sh", {":in", ":dir", ":env"});
    std::optional<std::string> input;
    if (const PO* in = option(ctx, opts, ":in"); in && in != PROTO_NONE) input = strOf(ctx, in);
    std::optional<std::string> dir;
    if (const PO* d = option(ctx, opts, ":dir"); d && d != PROTO_NONE) dir = pathArg(ctx, d, "sh");
    std::optional<std::vector<std::string>> env;
    if (const PO* e = option(ctx, opts, ":env"); e && e != PROTO_NONE) {
        if (!isMap(e)) wrongType(ctx, "sh :env", "a map", e);
        env.emplace();
        mapForEach(ctx, e, &*env, [](proto::ProtoContext* c, void* self, const PO* k, const PO* v) {
            std::string name = isString(k) ? bytesOf(c, k) : namedName(c, k);
            static_cast<std::vector<std::string>*>(self)->push_back(name + "=" + strOf(c, v));
        });
    }
    const protoio::process::RunResult r = blocking(ctx, [&] {
        if (!dir && !env) return protoio::process::run(argv, input);
        // posix_spawn has no working directory or fresh environment in
        // protoIO's run: go through `sh -c 'cd DIR && exec "$@"'` and
        // `env -i`, after checking what the JVM would refuse up front.
        if (dir) {
            const auto st = protoio::file::stat(*dir);
            if (!st || !st->isDirectory)
                throw protoio::Error(Kind::Process, "Cannot run program \"" + argv[0] +
                                     "\" (in directory \"" + *dir + "\"): No such file or directory", 2);
        }
#if defined(_WIN32)
        // `env -i` (Git for Windows') cannot search a PATH it has just
        // cleared: give it the program's full path.
        std::string found;
        if (!commandExists(argv[0], &found))
            throw protoio::Error(Kind::Process, "Cannot run program \"" + argv[0] +
                                 "\": No such file or directory", 2);
        if (env) argv[0] = found;
#else
        if (!commandExists(argv[0]))
            throw protoio::Error(Kind::Process, "Cannot run program \"" + argv[0] +
                                 "\": No such file or directory", 2);
#endif
        std::vector<std::string> full;
        if (dir) {
#if defined(_WIN32)
            // Windows has no /bin/sh: the `sh` and `env` on PATH (Git for
            // Windows ships both) do the same, until protoIO's run can set a
            // child's directory and environment itself.
            full = {"sh", "-c", "cd -- \"$0\" && exec \"$@\"", *dir};
#else
            full = {"/bin/sh", "-c", "cd -- \"$0\" && exec \"$@\"", *dir};
#endif
        }
        if (env) {
            full.push_back("env");
            full.push_back("-i");
            for (const std::string& kv : *env) full.push_back(kv);
        }
        full.insert(full.end(), argv.begin(), argv.end());
        return protoio::process::run(full, input);
    });
    const std::vector<const PO*> kv{
        kw(ctx, ":exit"), ctx->fromLong(r.exitCode),
        kw(ctx, ":out"),  makeString(ctx, r.out),
        kw(ctx, ":err"),  makeString(ctx, r.err),
    };
    return makeMap(ctx, kv);
}

// ---------------------------------------------------------------- the program

// (getenv name): the variable's value or nil; (getenv): every variable, as a
// map of strings.
IOPRIM(prim_getenv) {
    const proto::proto_ulong n = argCount(ctx, args);
    if (n > 1) arity(ctx, n, "getenv");
    if (n == 1) {
        const std::string name = stringArg(ctx, arg(ctx, args, 0), "getenv");
        const auto v = protoio::process::getenv(name);
        return v ? makeString(ctx, *v) : PROTO_NONE;
    }
    std::vector<const PO*> kv;
    for (const auto& [name, value] : protoio::process::environment()) {
        kv.push_back(makeString(ctx, name));
        kv.push_back(makeString(ctx, value));
    }
    return makeMap(ctx, kv);
}

// (exit) / (exit n): ends the process at once with status n (System/exit).
IOPRIM(prim_exit) {
    const proto::proto_ulong n = argCount(ctx, args);
    if (n > 1) arity(ctx, n, "exit");
    const int code = n == 1 ? static_cast<int>(longArg(ctx, arg(ctx, args, 0), "exit")) : 0;
    protoio::process::exit(code);
}

// ---------------------------------------------------------------- HTTP client

// babashka http-client's unexceptional statuses: any other status throws
// unless the request says :throw false.
bool unexceptional(int status) {
    switch (status) {
        case 200: case 201: case 202: case 203: case 204: case 205: case 206: case 207:
        case 300: case 301: case 302: case 303: case 304: case 307:
            return true;
        default:
            return false;
    }
}

std::string percentEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// The request described by `opts` (with `method` and `url` when given),
// performed; the response map, or an ExceptionInfo for an exceptional status.
const PO* httpCall(proto::ProtoContext* ctx, const char* who, const PO* opts,
                   std::optional<std::string> method, std::optional<std::string> url) {
    checkOptions(ctx, opts, who, {":method", ":uri", ":url", ":headers", ":body", ":timeout",
                                  ":throw", ":follow-redirects", ":query-params"});
    protoio::http::Request rq;
    if (!method) {
        const PO* m = option(ctx, opts, ":method");
        if (m && m != PROTO_NONE) {
            method = isString(m) ? bytesOf(ctx, m) : namedName(ctx, m);
            if (method->empty()) wrongType(ctx, who, "a keyword method", m);
        } else {
            method = "get";
        }
    }
    for (char& c : *method) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    rq.method = *method;
    if (!url) {
        const PO* u = option(ctx, opts, ":uri");
        if (!u || u == PROTO_NONE) u = option(ctx, opts, ":url");
        if (!u || u == PROTO_NONE)
            throwClassed(ctx, "IllegalArgumentException", std::string(who) + ": needs a :uri");
        url = stringArg(ctx, u, who);
    }
    rq.url = *url;
    if (const PO* qp = option(ctx, opts, ":query-params"); qp && qp != PROTO_NONE) {
        const protoio::http::Headers pairs = headersArg(ctx, qp, who);
        std::string q;
        for (const auto& p : pairs) {
            if (!q.empty()) q += '&';
            q += percentEncode(p.name) + "=" + percentEncode(p.value);
        }
        if (!q.empty()) rq.url += (rq.url.find('?') == std::string::npos ? "?" : "&") + q;
    }
    rq.headers = headersArg(ctx, option(ctx, opts, ":headers"), who);
    if (const PO* b = option(ctx, opts, ":body"); b && b != PROTO_NONE) rq.body = strOf(ctx, b);
    rq.timeoutMs = timeoutOption(ctx, opts, ":timeout", 30000, who);
    if (const PO* fr = option(ctx, opts, ":follow-redirects")) {
        if (!truthy(fr) || namedName(ctx, fr) == "never") rq.maxRedirects = 0;
    }
    const PO* throwOpt = option(ctx, opts, ":throw");
    const bool throwing = !throwOpt || truthy(throwOpt);

    const protoio::http::Response r = blocking(ctx, [&] { return protoio::http::httpRequest(rq); });

    std::vector<const PO*> hkv;
    for (const auto& h : r.headers) {
        hkv.push_back(makeString(ctx, h.name));
        hkv.push_back(makeString(ctx, h.value));
    }
    const std::vector<const PO*> kv{
        kw(ctx, ":status"),  ctx->fromLong(r.status),
        kw(ctx, ":headers"), makeMap(ctx, hkv),
        kw(ctx, ":body"),    makeString(ctx, r.body),
    };
    const PO* response = makeMap(ctx, kv);
    if (throwing && !unexceptional(r.status))
        throwClassed(ctx, "ExceptionInfo",
                     "Exceptional status code: " + std::to_string(r.status), response);
    return response;
}

// (http-request {:method :get :uri url ...}).
IOPRIM(prim_http_request) {
    const proto::proto_ulong n = argCount(ctx, args);
    if (n != 1) arity(ctx, n, "http-request");
    const PO* opts = arg(ctx, args, 0);
    if (!isMap(opts)) wrongType(ctx, "http-request", "a request map", opts);
    return httpCall(ctx, "http-request", opts, std::nullopt, std::nullopt);
}

const PO* httpVerb(proto::ProtoContext* ctx, const proto::ProtoList* args, const char* who,
                   const char* method) {
    const proto::proto_ulong n = argCount(ctx, args);
    if (n < 1) arity(ctx, n, who);
    const std::string url = stringArg(ctx, arg(ctx, args, 0), who);
    return httpCall(ctx, who, optionsArg(ctx, args, 1, who), std::string(method), url);
}

IOPRIM(prim_http_get) { return httpVerb(ctx, args, "http-get", "GET"); }
IOPRIM(prim_http_post) { return httpVerb(ctx, args, "http-post", "POST"); }
IOPRIM(prim_http_put) { return httpVerb(ctx, args, "http-put", "PUT"); }
IOPRIM(prim_http_delete) { return httpVerb(ctx, args, "http-delete", "DELETE"); }
IOPRIM(prim_http_head) { return httpVerb(ctx, args, "http-head", "HEAD"); }

// ---------------------------------------------------------------- HTTP server

// (run-server handler {:port 8080}): serves HTTP/1.1 on `port` (0: a free
// one, see server-port), calling (handler request) for each request on a
// thread of its own; answers the server.
IOPRIM(prim_run_server) {
    const proto::proto_ulong n = argCount(ctx, args);
    if (n < 1) arity(ctx, n, "run-server");
    const PO* handler = arg(ctx, args, 0);
    if (handler == PROTO_NONE) wrongType(ctx, "run-server", "a handler fn", handler);
    const PO* opts = optionsArg(ctx, args, 1, "run-server");
    checkOptions(ctx, opts, "run-server",
                 {":port", ":host", ":ip", ":max-body", ":read-timeout", ":max-line"});
    int port = 8080;
    if (const PO* p = option(ctx, opts, ":port"); p && p != PROTO_NONE) port = portArg(ctx, p, "run-server");
    std::string host;
    if (const PO* h = option(ctx, opts, ":host"); h && h != PROTO_NONE) host = stringArg(ctx, h, "run-server");
    if (const PO* h = option(ctx, opts, ":ip"); h && h != PROTO_NONE) host = stringArg(ctx, h, "run-server");
    auto server = std::make_unique<HttpServer>();
    if (const PO* mb = option(ctx, opts, ":max-body"); mb && mb != PROTO_NONE) {
        const long long v = longArg(ctx, mb, "run-server");
        server->limits.maxBody = v < 0 ? 0 : static_cast<std::size_t>(v);
    }
    if (const PO* ml = option(ctx, opts, ":max-line"); ml && ml != PROTO_NONE)
        server->limits.maxLine = static_cast<std::size_t>(std::max(64LL, longArg(ctx, ml, "run-server")));
    server->readTimeoutMs = timeoutOption(ctx, opts, ":read-timeout", 30000, "run-server");
    server->cc = callContext();
    server->host = host;
    const std::pair<int, int> bound = blocking(ctx, [&] {
        const int fd = protoio::net::tcpListen(listenHost(host), port);
        return std::make_pair(fd, protoio::net::sockName(fd).port);
    });
    server->listenFd = bound.first;
    server->port = bound.second;
    server->handler = handler;
    server->pin = g_ioRoots->add(handler);
    HttpServer* s = server.get();
    {
        std::lock_guard<std::mutex> lock(g_serversMutex);
        g_servers.push_back(std::move(server));
    }
    const PO* h = newHandle(ctx, HandleKind::HttpServer, -1, host.empty() ? "0.0.0.0" : host);
    h->setAttribute(ctx, serverKey(ctx), ctx->fromLong(reinterpret_cast<long long>(s)));
    const proto::ProtoList* targs =
        ctx->newList()->appendLast(ctx, ctx->fromLong(reinterpret_cast<long long>(s)));
    s->acceptThread = ctx->space->newThread(
        ctx, proto::ProtoString::createSymbol(ctx, "protoclj-http-accept"), &acceptMain, targs,
        nullptr);
    return h;
}

// (stop-server s): stops accepting, lets requests in progress finish and
// waits for them; nil.
IOPRIM(prim_stop_server) {
    const proto::proto_ulong n = argCount(ctx, args);
    if (n != 1) arity(ctx, n, "stop-server");
    stopServer(ctx, serverOf(ctx, arg(ctx, args, 0), "stop-server"));
    return PROTO_NONE;
}

// (server-port s): the local port of an HTTP server, a listening socket, a
// UDP socket or a connected socket.
IOPRIM(prim_server_port) {
    const proto::proto_ulong n = argCount(ctx, args);
    if (n != 1) arity(ctx, n, "server-port");
    const PO* h = arg(ctx, args, 0);
    if (isHandle(ctx, h) && handleKind(ctx, h) == HandleKind::HttpServer)
        return ctx->fromLong(serverOf(ctx, h, "server-port")->port);
    const int fd = openFd(ctx, h, "server-port",
                          {HandleKind::ServerSocket, HandleKind::UdpSocket, HandleKind::Socket});
    const int port = blocking(ctx, [&] { return protoio::net::sockName(fd).port; });
    return ctx->fromLong(port);
}

// ---------------------------------------------------------------- sockets

// (tcp-connect host port & {:keys [tls timeout verify]}): a connected socket.
// :timeout (ms) bounds the connect and every later wait on the socket; :tls
// true upgrades it to TLS (the certificate is verified unless :verify false).
IOPRIM(prim_tcp_connect) {
    const proto::proto_ulong n = argCount(ctx, args);
    if (n < 2) arity(ctx, n, "tcp-connect");
    const std::string host = stringArg(ctx, arg(ctx, args, 0), "tcp-connect");
    const int port = portArg(ctx, arg(ctx, args, 1), "tcp-connect");
    const PO* opts = optionsArg(ctx, args, 2, "tcp-connect");
    checkOptions(ctx, opts, "tcp-connect", {":tls", ":timeout", ":verify"});
    const bool tls = truthy(option(ctx, opts, ":tls"));
    const PO* verifyOpt = option(ctx, opts, ":verify");
    const bool verify = !verifyOpt || truthy(verifyOpt);
    const int timeout = timeoutOption(ctx, opts, ":timeout", -1, "tcp-connect");
    const int fd = blocking(ctx, [&] {
        const int s = protoio::net::tcpConnect(host, port, timeout);
        try {
            protoio::setTimeout(s, timeout);
            if (tls) protoio::net::tlsConnect(s, host, verify);
        } catch (...) {
            protoio::close(s);
            throw;
        }
        return s;
    });
    return newHandle(ctx, HandleKind::Socket, fd, host + ":" + std::to_string(port));
}

// (tcp-listen port) / (tcp-listen host port): a listening socket (an empty
// host, the default, listens on every interface; port 0 picks a free port).
IOPRIM(prim_tcp_listen) {
    const proto::proto_ulong n = argCount(ctx, args);
    if (n < 1 || n > 2) arity(ctx, n, "tcp-listen");
    const std::string host = n == 2 ? stringArg(ctx, arg(ctx, args, 0), "tcp-listen") : "";
    const int port = portArg(ctx, arg(ctx, args, n - 1), "tcp-listen");
    const std::pair<int, int> bound = blocking(ctx, [&] {
        const int fd = protoio::net::tcpListen(listenHost(host), port);
        return std::make_pair(fd, protoio::net::sockName(fd).port);
    });
    return newHandle(ctx, HandleKind::ServerSocket, bound.first,
                     (host.empty() ? std::string("0.0.0.0") : host) + ":" + std::to_string(bound.second));
}

// (tcp-accept server) / (tcp-accept server timeout-ms): the next connection,
// or nil when the timeout elapsed first or the socket was closed.
IOPRIM(prim_tcp_accept) {
    const proto::proto_ulong n = argCount(ctx, args);
    if (n < 1 || n > 2) arity(ctx, n, "tcp-accept");
    const int fd = openFd(ctx, arg(ctx, args, 0), "tcp-accept", {HandleKind::ServerSocket});
    const int timeout = n == 2 ? static_cast<int>(longArg(ctx, arg(ctx, args, 1), "tcp-accept")) : -1;
    const std::optional<std::pair<int, std::string>> c = blocking(ctx, [&] {
        std::optional<std::pair<int, std::string>> out;
        if (auto s = protoio::net::tcpAccept(fd, timeout)) {
            std::string peer;
            try {
                const auto a = protoio::net::peerName(*s);
                peer = a.host + ":" + std::to_string(a.port);
            } catch (const protoio::Error&) {}
            out.emplace(*s, peer);
        }
        return out;
    });
    if (!c) return PROTO_NONE;
    return newHandle(ctx, HandleKind::Socket, c->first, c->second);
}

// (udp-socket) / (udp-socket port) / (udp-socket host port): a bound UDP
// socket.
IOPRIM(prim_udp_socket) {
    const proto::proto_ulong n = argCount(ctx, args);
    if (n > 2) arity(ctx, n, "udp-socket");
    const std::string host = n == 2 ? stringArg(ctx, arg(ctx, args, 0), "udp-socket") : "";
    const int port = n >= 1 ? portArg(ctx, arg(ctx, args, n - 1), "udp-socket") : 0;
    const std::pair<int, int> bound = blocking(ctx, [&] {
        const int fd = protoio::net::udpBind(host, port);
        return std::make_pair(fd, protoio::net::sockName(fd).port);
    });
    return newHandle(ctx, HandleKind::UdpSocket, bound.first,
                     (host.empty() ? std::string("0.0.0.0") : host) + ":" + std::to_string(bound.second));
}

// (udp-send sock host port data): sends one datagram with (str data); nil.
IOPRIM(prim_udp_send) {
    const proto::proto_ulong n = argCount(ctx, args);
    if (n != 4) arity(ctx, n, "udp-send");
    const int fd = openFd(ctx, arg(ctx, args, 0), "udp-send", {HandleKind::UdpSocket});
    const std::string host = stringArg(ctx, arg(ctx, args, 1), "udp-send");
    const int port = portArg(ctx, arg(ctx, args, 2), "udp-send");
    const std::string data = strOf(ctx, arg(ctx, args, 3));
    blocking(ctx, [&] { protoio::net::udpSend(fd, host, port, data); });
    return PROTO_NONE;
}

// (udp-receive sock) / (udp-receive sock timeout-ms): the next datagram as
// {:data "..." :host "..." :port n}, or nil when the timeout elapsed first.
IOPRIM(prim_udp_receive) {
    const proto::proto_ulong n = argCount(ctx, args);
    if (n < 1 || n > 2) arity(ctx, n, "udp-receive");
    const int fd = openFd(ctx, arg(ctx, args, 0), "udp-receive", {HandleKind::UdpSocket});
    const int timeout = n == 2 ? static_cast<int>(longArg(ctx, arg(ctx, args, 1), "udp-receive")) : -1;
    const std::optional<protoio::net::Datagram> d =
        blocking(ctx, [&] { return protoio::net::udpReceive(fd, timeout); });
    if (!d) return PROTO_NONE;
    const std::vector<const PO*> kv{
        kw(ctx, ":data"), makeString(ctx, d->data),
        kw(ctx, ":host"), makeString(ctx, d->host),
        kw(ctx, ":port"), ctx->fromLong(d->port),
    };
    return makeMap(ctx, kv);
}

#undef IOPRIM

struct IOEntry {
    const char* name;
    proto::ProtoMethod fn;
};

constexpr IOEntry kIOPrimitives[] = {
    // Core (clojure.core, clojure.java.io).
    {"slurp",        &prim_slurp},
    {"spit",         &prim_spit},
    {"read-line",    &prim_read_line},
    {"write",        &prim_write},
    {"close",        &prim_close},
    {"reader",       &prim_reader},
    {"writer",       &prim_writer},
    {"line-seq",     &prim_line_seq},
    {"file",         &prim_file},
    {"file-seq",     &prim_file_seq},
    {"exists?",      &prim_exists_p},
    {"directory?",   &prim_directory_p},
    {"delete-file",  &prim_delete_file},
    {"make-parents", &prim_make_parents},
    {"copy",         &prim_copy},
    // Other programs and the running one (clojure.java.shell, System).
    {"sh",           &prim_sh},
    {"getenv",       &prim_getenv},
    {"exit",         &prim_exit},
    // HTTP (babashka http-client, Ring).
    {"http-request", &prim_http_request},
    {"http-get",     &prim_http_get},
    {"http-post",    &prim_http_post},
    {"http-put",     &prim_http_put},
    {"http-delete",  &prim_http_delete},
    {"http-head",    &prim_http_head},
    {"run-server",   &prim_run_server},
    {"stop-server",  &prim_stop_server},
    {"server-port",  &prim_server_port},
    // Sockets (protoClojure's own, deviation D30).
    {"tcp-connect",      &prim_tcp_connect},
    {"tcp-listen",       &prim_tcp_listen},
    {"tcp-accept",       &prim_tcp_accept},
    {"socket-read-line", &prim_read_line},
    {"socket-write",     &prim_write},
    {"socket-close",     &prim_close},
    {"udp-socket",       &prim_udp_socket},
    {"udp-send",         &prim_udp_send},
    {"udp-receive",      &prim_udp_receive},
    // What `with-open` calls, under a name a program cannot shadow by
    // defining its own `close` (Compiler.cpp).
    {"__with_open_close__", &prim_close},
};

} // namespace

// ================================================================= public

void installIO(proto::ProtoContext* ctx, proto::ProtoObject* globals) {
    const PO* marker = ctx->space->objectPrototype->newChild(ctx);
    globals->setAttribute(ctx, proto::ProtoString::createSymbol(ctx, "__io_marker__"), marker);
    g_ioMarker = marker;
    if (!g_ioRoots) g_ioRoots = ctx->space->createRootSet("protoclj-io-servers");
    for (const IOEntry& p : kIOPrimitives) {
        globals->setAttribute(ctx, proto::ProtoString::createSymbol(ctx, p.name),
                              ctx->fromMethod(nullptr, p.fn));
    }
    globals->setAttribute(ctx, proto::ProtoString::createSymbol(ctx, "*command-line-args*"),
                          PROTO_NONE);
}

void setCommandLineArgs(proto::ProtoContext* ctx, proto::ProtoObject* globals,
                        const std::vector<std::string>& args) {
    const proto::ProtoString* key = proto::ProtoString::createSymbol(ctx, "*command-line-args*");
    if (args.empty()) {
        globals->setAttribute(ctx, key, PROTO_NONE);
        return;
    }
    const proto::ProtoList* l = ctx->newList();
    for (const std::string& a : args) l = l->appendLast(ctx, makeString(ctx, a));
    globals->setAttribute(ctx, key, l->asObject(ctx));
}

void shutdownIO(proto::ProtoContext* ctx) {
    std::vector<HttpServer*> servers;
    {
        std::lock_guard<std::mutex> lock(g_serversMutex);
        for (const auto& s : g_servers) servers.push_back(s.get());
    }
    for (HttpServer* s : servers) stopServer(ctx, s);
}

bool appendIOHandle(proto::ProtoContext* ctx, std::string& out, const proto::ProtoObject* v) {
    if (!isHandle(ctx, v)) return false;
    const HandleKind k = handleKind(ctx, v);
    out += "#<";
    out += kHandleNames[static_cast<int>(k)];
    if (k == HandleKind::HttpServer) {
        const PO* p = v->getAttribute(ctx, serverKey(ctx));
        out += ' ';
        out += std::to_string(reinterpret_cast<HttpServer*>(p->asLong(ctx))->port);
    } else {
        const std::string path = handlePath(ctx, v);
        if (!path.empty()) {
            out += ' ';
            out += path;
        }
        if (k != HandleKind::File && handleFd(ctx, v) < 0) out += " closed";
    }
    out += '>';
    return true;
}

bool appendIOFilePath(proto::ProtoContext* ctx, std::string& out, const proto::ProtoObject* v) {
    if (!isHandle(ctx, v) || handleKind(ctx, v) != HandleKind::File) return false;
    out += handlePath(ctx, v);
    return true;
}

const char* ioHandleTypeName(proto::ProtoContext* ctx, const proto::ProtoObject* v) {
    if (!isHandle(ctx, v)) return nullptr;
    return kHandleTypeNames[static_cast<int>(handleKind(ctx, v))];
}

const char* ioPrimitiveName(const void* fn) {
    for (const IOEntry& p : kIOPrimitives) {
        if (reinterpret_cast<const void*>(p.fn) == fn && p.name[0] != '_') return p.name;
    }
    return nullptr;
}

} // namespace protoClojure
