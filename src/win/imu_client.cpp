#include "imu_client.h"
#include <cstdlib>

namespace ox {

static bool parseAddress(const std::string& s, sockaddr_in& out) {
    std::string host = s;
    int port = 52998;
    size_t c = s.rfind(':');
    if (c != std::string::npos) {
        host = s.substr(0, c);
        port = atoi(s.c_str() + c + 1);
    }
    if (port <= 0 || port > 65535) return false;
    out = {};
    out.sin_family = AF_INET;
    out.sin_port = htons((u_short)port);
    return inet_pton(AF_INET, host.c_str(), &out.sin_addr) == 1;
}

void ImuClient::start(const std::string& address) {
    if (running_) return;
    running_ = true;
    thread_ = std::thread([this, address] { run(address); });
}

void ImuClient::stop() {
    if (!running_) return;
    running_ = false;
    SOCKET s = sock_;
    if (s != INVALID_SOCKET) shutdown(s, SD_BOTH);
    if (thread_.joinable()) thread_.join();
    connected_ = false;
    rate_ = 0;
}

bool ImuClient::ready() {
    std::lock_guard<std::mutex> lk(m_);
    return connected_ && tracker_.filter.ready();
}

Quat ImuClient::view(float frameDt) {
    std::lock_guard<std::mutex> lk(m_);
    return tracker_.viewOrientation(frameDt);
}

void ImuClient::recenter() {
    std::lock_guard<std::mutex> lk(m_);
    tracker_.recenter();
}

void ImuClient::startCalibration() {
    std::lock_guard<std::mutex> lk(m_);
    wizard_.start();
    // the uncalibrated mode assumes the accelerometer points up; if the user had
    // to invert it, it's the other way round on these glasses
    wizard_.expectedAccelSign = tracker_.filter.invertYaw ? -1.0f : 1.0f;
    wizardPending_ = true;
}

bool ImuClient::calibrationFinished(AxisCalibration& cal, bool& ok, CalibrationWizard::Error& err) {
    std::lock_guard<std::mutex> lk(m_);
    if (!wizardPending_ || wizard_.active()) return false;
    wizardPending_ = false;
    ok = wizard_.step() == CalibrationWizard::Done;
    err = wizard_.error();
    cal = wizard_.result();
    if (ok) logf("calibration: done%s", wizard_.turnedLeft() ? ", turned left (fixed)" : "");
    else logf("calibration: failed (%d)", (int)err);
    if (ok) {
        tracker_.filter.setCalibration(cal);
        tracker_.filter.setGyroBias(wizard_.measuredBias());
        tracker_.recenter();
    }
    return true;
}

void ImuClient::run(std::string address) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    sockaddr_in addr;
    if (!parseAddress(address, addr)) {
        logf("imu: bad address '%s'", address.c_str());
        WSACleanup();
        return;
    }
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    std::vector<uint8_t> buf(16384);
    ImuStream stream;
    int attempts = 0;

    while (running_) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) { Sleep(1000); continue; }
        // non blocking connect so we can time out after 2 s
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        connect(s, (sockaddr*)&addr, sizeof(addr));
        fd_set wr, ex;
        FD_ZERO(&wr); FD_SET(s, &wr);
        FD_ZERO(&ex); FD_SET(s, &ex);
        timeval tv = {2, 0};
        bool ok = select(0, nullptr, &wr, &ex, &tv) > 0 && FD_ISSET(s, &wr) && !FD_ISSET(s, &ex);
        if (ok) {
            int soerr = 0, len = sizeof(soerr);
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soerr, &len);
            ok = soerr == 0;
        }
        if (!ok) {
            closesocket(s);
            if (attempts++ % 15 == 0) logf("imu: can't reach %s, retrying", address.c_str());
            for (int i = 0; i < 10 && running_; i++) Sleep(100);
            continue;
        }
        nb = 0;
        ioctlsocket(s, FIONBIO, &nb);
        DWORD timeout = 2000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        sock_ = s;
        attempts = 0;
        logf("imu: connected to %s", address.c_str());
        stream.reset();
        {
            std::lock_guard<std::mutex> lk(m_);
            tracker_.filter.reset();
            tracker_.recenter();
        }
        QueryPerformanceCounter(&now);
        LARGE_INTEGER rateStart = now;
        uint64_t count = 0;
        double lastRead = 0;

        while (running_) {
            int n = recv(s, (char*)buf.data(), (int)buf.size(), 0);
            if (n <= 0) {
                logf("imu: connection lost (%d, %d)", n, WSAGetLastError());
                break;
            }
            QueryPerformanceCounter(&now);
            double t = (double)now.QuadPart / (double)freq.QuadPart;
            if (lastRead > 0) {
                float gap = (float)((t - lastRead) * 1000.0);
                if (gap > maxGapMs_) maxGapMs_ = gap;
            }
            lastRead = t;
            int got;
            {
                std::lock_guard<std::mutex> lk(m_);
                got = stream.onBytes(buf.data(), (size_t)n, t, [&](const ImuSample& smp, float dt) {
                    tracker_.ingest(smp, dt);
                    wizard_.update(smp, dt);
                });
            }
            if (got) connected_ = true;
            count += (uint64_t)got;
            double elapsed = (double)(now.QuadPart - rateStart.QuadPart) / (double)freq.QuadPart;
            if (elapsed >= 1.0) {
                rate_ = (float)(count / elapsed);
                count = 0;
                rateStart = now;
            }
        }
        sock_ = INVALID_SOCKET;
        closesocket(s);
        connected_ = false;
        rate_ = 0;
        for (int i = 0; i < 5 && running_; i++) Sleep(100);
    }
    WSACleanup();
}

} // namespace ox
