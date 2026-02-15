#pragma once

#include "alvr_server/IDRScheduler.h"
#include "shared/threadtools.h"
#include <atomic>
#include <memory>
#include <poll.h>
#include <vector>
#include <sys/types.h>

class PoseHistory;

class CEncoder : public CThread {
public:
    CEncoder(std::shared_ptr<PoseHistory> poseHistory);
    ~CEncoder();
    bool Init() override { return true; }
    void Run() override;

    void Stop();
    void OnStreamStart();
    void InsertIDR();
    bool IsConnected() { return m_connected; }
    void CaptureFrame();

private:
    void GetFds(int client, size_t fd_count, std::vector<int>& fds);
    std::shared_ptr<PoseHistory> m_poseHistory;
    std::atomic_bool m_exiting { false };
    IDRScheduler m_scheduler;
    pollfd m_socket;
    std::string m_socketPath;
    std::vector<int> m_fds;
    bool m_connected = false;
    std::atomic_bool m_captureFrame = false;
};
