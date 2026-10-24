#include "dap/DapStdio.h"
#include "protoCore.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <streambuf>
#include <thread>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace protoClojure {

namespace {

#if defined(_WIN32)
long readFd(int fd, char* buf, size_t n)        { return _read(fd, buf, static_cast<unsigned>(n)); }
long writeFd(int fd, const char* buf, size_t n) { return _write(fd, buf, static_cast<unsigned>(n)); }
int  dupFd(int fd)                              { return _dup(fd); }
int  dup2Fd(int from, int to)                   { return _dup2(from, to); }
void closeFd(int fd)                            { _close(fd); }
int  openNull(bool forWrite) {
    return _open("NUL", (forWrite ? _O_WRONLY : _O_RDONLY) | _O_BINARY);
}
bool makePipe(int fds[2])                       { return _pipe(fds, 65536, _O_BINARY | _O_NOINHERIT) == 0; }
void keepPrivate(int fd)                        { _setmode(fd, _O_BINARY); }
#else
long readFd(int fd, char* buf, size_t n)        { return static_cast<long>(::read(fd, buf, n)); }
long writeFd(int fd, const char* buf, size_t n) { return static_cast<long>(::write(fd, buf, n)); }
int  dupFd(int fd)                              { return ::dup(fd); }
int  dup2Fd(int from, int to)                   { return ::dup2(from, to); }
void closeFd(int fd)                            { ::close(fd); }
int  openNull(bool forWrite)                    { return ::open("/dev/null", forWrite ? O_WRONLY : O_RDONLY); }
bool makePipe(int fds[2])                       { return ::pipe(fds) == 0; }
// A process the script spawns must not inherit the protocol's descriptors or
// the pipes' read ends.
void keepPrivate(int fd)                        { ::fcntl(fd, F_SETFD, FD_CLOEXEC); }
#endif

// A streambuf over a raw descriptor, for the protocol's private duplicates.
class FdBuf : public std::streambuf {
public:
    explicit FdBuf(int fd) : fd_(fd) {
        setg(in_, in_, in_);
        setp(out_, out_ + sizeof out_);
    }

protected:
    int_type underflow() override {
        if (gptr() < egptr())
            return traits_type::to_int_type(*gptr());
        long n;
        do { n = readFd(fd_, in_, sizeof in_); } while (n < 0 && errno == EINTR);
        if (n <= 0)
            return traits_type::eof();
        setg(in_, in_, in_ + n);
        return traits_type::to_int_type(*gptr());
    }

    int_type overflow(int_type c) override {
        if (!flushOut())
            return traits_type::eof();
        if (!traits_type::eq_int_type(c, traits_type::eof())) {
            *pptr() = traits_type::to_char_type(c);
            pbump(1);
        }
        return traits_type::not_eof(c);
    }

    int sync() override { return flushOut() ? 0 : -1; }

private:
    bool flushOut() {
        const char* p = pbase();
        while (p < pptr()) {
            long n = writeFd(fd_, p, static_cast<size_t>(pptr() - p));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            p += n;
        }
        setp(out_, out_ + sizeof out_);
        return true;
    }

    int  fd_;
    char in_[4096];
    char out_[4096];
};

// Length of `s` without an incomplete UTF-8 sequence at its end, so a chunk
// never splits a character (the JSON of an `output` event must be valid UTF-8).
size_t completeUtf8Prefix(const std::string& s) {
    size_t n = s.size();
    for (size_t back = 1; back <= 3 && back <= n; ++back) {
        unsigned char c = static_cast<unsigned char>(s[n - back]);
        if ((c & 0xC0) == 0x80) continue;            // continuation byte
        size_t len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
        return len > back ? n - back : n;
    }
    return n;
}

// What waitForwarded writes to fds 1 and 2 and the pumps take out of the
// stream: when a pump meets it, everything written before it has gone
// through the sink. A program printing these bytes itself is not expected.
const char kSyncMarkBytes[] = "\0\x1f" "protoclj-dap-sync" "\x1f\0";
const std::string kSyncMark(kSyncMarkBytes, sizeof kSyncMarkBytes - 1);

// The length of the longest end of `s` that begins the mark: bytes a pump
// holds back until the next read tells whether the mark follows.
size_t partialMarkAtEnd(const std::string& s) {
    for (size_t k = std::min(s.size(), kSyncMark.size() - 1); k > 0; --k)
        if (s.compare(s.size() - k, k, kSyncMark, 0, k) == 0) return k;
    return 0;
}

} // namespace

struct DapStdio::Impl {
    bool redirected = false;
    int  protoIn  = -1;       // duplicate of the original fd 0
    int  protoOut = -1;       // duplicate of the original fd 1
    int  savedErr = -1;       // duplicate of the original fd 2
    int  outRead  = -1;       // read end of the pipe now behind fd 1
    int  errRead  = -1;       // read end of the pipe now behind fd 2
    std::unique_ptr<FdBuf>        inBuf, outBuf;
    std::unique_ptr<std::istream> in;
    std::unique_ptr<std::ostream> out;
    std::thread outPump, errPump;
    // Sync marks requested and met per pump (waitForwarded).
    std::mutex              syncMu;
    std::condition_variable syncCv;
    unsigned                syncRequested = 0;
    unsigned                syncSeen[2] = {0, 0};   // stdout, stderr
    std::mutex  finishMu;
    bool        finished = false;

    // Points `fd` at a fresh pipe and answers its read end (-1 on failure).
    static int capture(int fd) {
        int fds[2];
        if (!makePipe(fds)) return -1;
        if (dup2Fd(fds[1], fd) < 0) { closeFd(fds[0]); closeFd(fds[1]); return -1; }
        closeFd(fds[1]);                              // fd now holds the write end
        keepPrivate(fds[0]);
        return fds[0];
    }

    void setUp() {
        std::fflush(stdout);
        std::fflush(stderr);
        protoIn  = dupFd(0);
        protoOut = dupFd(1);
        savedErr = dupFd(2);
        int nullIn = openNull(false);
        if (protoIn < 0 || protoOut < 0 || savedErr < 0 || nullIn < 0) {
            for (int fd : {protoIn, protoOut, savedErr, nullIn}) if (fd >= 0) closeFd(fd);
            protoIn = protoOut = savedErr = -1;
            return;
        }
        keepPrivate(protoIn);
        keepPrivate(protoOut);
        keepPrivate(savedErr);
        outRead = capture(1);
        errRead = capture(2);
        dup2Fd(nullIn, 0);
        closeFd(nullIn);
        redirected = true;

        inBuf  = std::make_unique<FdBuf>(protoIn);
        outBuf = std::make_unique<FdBuf>(protoOut);
        in     = std::make_unique<std::istream>(inBuf.get());
        out    = std::make_unique<std::ostream>(outBuf.get());
    }

    void pump(int fd, int which, const char* category, const Sink& sink) {
        std::string pending;
        char buf[4096];
        for (;;) {
            long n = readFd(fd, buf, sizeof buf);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;                        // every writer closed
            pending.append(buf, static_cast<size_t>(n));
            for (size_t at; (at = pending.find(kSyncMark)) != std::string::npos;) {
                if (at > 0) sink(category, pending.substr(0, at));
                pending.erase(0, at + kSyncMark.size());
                {
                    std::lock_guard<std::mutex> lk(syncMu);
                    ++syncSeen[which];
                }
                syncCv.notify_all();
            }
            const size_t ready = pending.size() - partialMarkAtEnd(pending);
            size_t cut = completeUtf8Prefix(pending.substr(0, ready));
            if (cut == 0) continue;
            sink(category, pending.substr(0, cut));
            pending.erase(0, cut);
        }
        if (!pending.empty())
            sink(category, pending);
    }
};

DapStdio::DapStdio() : impl_(std::make_unique<Impl>()) { impl_->setUp(); }

DapStdio::~DapStdio() {
    finish(nullptr);
    for (int fd : {impl_->protoIn, impl_->protoOut, impl_->savedErr})
        if (fd >= 0) closeFd(fd);
}

std::istream& DapStdio::protocolIn()  { return impl_->in  ? *impl_->in  : std::cin;  }
std::ostream& DapStdio::protocolOut() { return impl_->out ? *impl_->out : std::cout; }

void DapStdio::startForwarding(Sink sink) {
    if (!impl_->redirected) return;
    if (impl_->outRead >= 0)
        impl_->outPump = std::thread(&Impl::pump, impl_.get(), impl_->outRead, 0, "stdout", sink);
    if (impl_->errRead >= 0)
        impl_->errPump = std::thread(&Impl::pump, impl_.get(), impl_->errRead, 1, "stderr", sink);
}

void DapStdio::waitForwarded() {
    std::lock_guard<std::mutex> fin(impl_->finishMu);
    if (!impl_->redirected || impl_->finished) return;
    // A mark behind what was written so far, on each stream; when both pumps
    // have met theirs, the bytes before them went through the sink. Bounded,
    // in case a pump is stuck behind a sink that cannot write.
    std::fflush(stdout);
    std::fflush(stderr);
    std::unique_lock<std::mutex> lk(impl_->syncMu);
    const unsigned target = ++impl_->syncRequested;
    const bool out = impl_->outPump.joinable(), err = impl_->errPump.joinable();
    if (out) writeFd(1, kSyncMark.data(), kSyncMark.size());
    if (err) writeFd(2, kSyncMark.data(), kSyncMark.size());
    impl_->syncCv.wait_for(lk, std::chrono::seconds(2), [&] {
        return (!out || impl_->syncSeen[0] >= target) && (!err || impl_->syncSeen[1] >= target);
    });
}

void DapStdio::finish(proto::ProtoContext* ctx) {
    std::lock_guard<std::mutex> lk(impl_->finishMu);
    if (!impl_->redirected || impl_->finished) return;
    impl_->finished = true;

    std::cout.flush();
    std::cerr.flush();
    std::fflush(stdout);
    std::fflush(stderr);
    // Releasing fds 1 and 2 closes the pipes' last write ends: each pump reads
    // what is left and stops at end of file.
    int nullOut = openNull(true);
    if (nullOut >= 0) {
        dup2Fd(nullOut, 1);
        closeFd(nullOut);
    }
    dup2Fd(impl_->savedErr, 2);
    // The pumps are plain threads, but the caller may be registered: blocked
    // here it must not hold up a stop-the-world.
    proto::ProtoContext::UnmanagedScope unmanaged(ctx);
    if (impl_->outPump.joinable()) impl_->outPump.join();
    if (impl_->errPump.joinable()) impl_->errPump.join();
    for (int* fd : {&impl_->outRead, &impl_->errRead})
        if (*fd >= 0) { closeFd(*fd); *fd = -1; }
}

} // namespace protoClojure
