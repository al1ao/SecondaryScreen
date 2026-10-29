#include "app/host_service.h"
#include "display/monitor_enum.h"
#include "util/log.h"
namespace pds {
HostService::~HostService(){Stop();}
void HostService::Stop(){if(worker_.joinable()){worker_.request_stop();worker_.join();}}
void HostService::Start(SessionOptions options) {
    Stop();running_.store(true);
    worker_=std::jthread([this,options=std::move(options)](std::stop_token stop) mutable {
        try {
            WinsockRuntime winsock;
            const auto monitors=EnumerateMonitors();
            const auto selected=SelectTargetMonitor(monitors,options.display_match,options.capture_primary,options.monitor_id);
            LogMonitors(monitors,selected?selected->hmonitor:nullptr);
            if(!winsock.Ready() || !selected){LogMissingVirtualMonitor();running_.store(false);return;}
            if(!options.capture_primary)options.monitor_id=selected->monitor_device_id;
            TcpServer server;
            if(!server.Listen(options.port)){running_.store(false);return;}
            LOG_INFO("Service listening on 127.0.0.1:",options.port);
            std::unique_ptr<PipelineSession> session;
            while(!stop.stop_requested()) {
                if(session && !session->Running())session.reset();
                if(!Readable(server.Socket(),200))continue;
                std::string peer;UniqueSocket client(server.Accept(peer));
                if(client.Get()==INVALID_SOCKET)continue;
                session.reset();
                LOG_INFO("client connected: ",peer);
                session=std::make_unique<PipelineSession>(std::move(client),options);session->Run();
            }
        } catch(const std::exception& error){LOG_ERROR("Host service: ",error.what());}
        running_.store(false);
    });
}
}
