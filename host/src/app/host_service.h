#pragma once
#include "net/pipeline_session.h"
#include <atomic>
#include <thread>
namespace pds {
// Owns the existing PipelineSession; it never replaces capture/encode workers.
class HostService {
public:
    ~HostService();
    void Start(SessionOptions options);
    void Stop();
    bool Running() const {return running_.load();}
private:
    std::jthread worker_;
    std::atomic<bool> running_{false};
};
}
