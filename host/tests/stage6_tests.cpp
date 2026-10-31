#include "app/cli_options.h"
#include "app/config.h"
#include "capture/frame_pacer.h"
#include "capture/wgc_capture.h"
#include "display/adaptive_mode.h"
#include "util/log.h"
#include <array>
#include <fstream>
#include <stdexcept>
#include <vector>
namespace {
void Check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
pds::SessionOptions Parse(std::initializer_list<const char*> arguments) {
    std::vector<std::string> text{"host"}; for (const auto* arg : arguments) text.emplace_back(arg);
    std::vector<char*> argv; for (auto& arg : text) argv.push_back(arg.data());
    pds::SessionOptions options;
    Check(pds::ParseSessionOptions(static_cast<int>(argv.size()), argv.data(), options), "parse valid CLI");
    return options;
}
void RequestTest() {
    pds::Hello hello{1,0,2944,1840,144,320,4096,4096,3};
    auto options = Parse({});
    auto mode = pds::ResolveVideoRequest(options, hello);
    Check(mode.width == 2944 && mode.height == 1840 && mode.fps == 144 && options.auto_resize, "default HELLO native size/rate");
    options = Parse({"--width","1281","--max-fps","60"});
    mode = pds::ResolveVideoRequest(options, hello);
    Check(mode.width == 1280 && mode.height == 1840 && mode.fps == 60, "explicit width alone, even and cap");
    options = Parse({"--width","1920","--height","1200","--fps","120","--max-fps","90"});
    mode = pds::ResolveVideoRequest(options, hello);
    Check(mode.width == 1920 && mode.height == 1200 && mode.fps == 90, "explicit size and FPS priority, then cap");
    options = Parse({"--no-auto-resize","--max-fps","0"});
    Check(!options.auto_resize && pds::ResolveVideoRequest(options,hello).fps == 144, "disable resize independent of unlimited FPS");
    Check(!Parse({"--capture-primary"}).auto_resize, "primary debug does not resize the desktop");
    Check(!Parse({"--auto-resize","--no-auto-resize"}).auto_resize, "last resize switch wins");
    Check(Parse({"--no-auto-resize","--auto-resize"}).auto_resize, "resize can be reenabled");
    std::string program="host", flag="--max-fps", bad="166";
    char* argv[]{program.data(),flag.data(),bad.data()}; pds::SessionOptions rejected;
    Check(!pds::ParseSessionOptions(3,argv,rejected), "invalid max FPS rejected");
    for (const auto dimension : {0u,1u,16385u,UINT32_MAX}) {
        hello.preferred_width=dimension;hello.preferred_height=dimension;
        mode=pds::ResolveVideoRequest(Parse({}),hello);
        Check(mode.width==1280 && mode.height==800, "invalid request uses built-in defaults");
    }
    hello.preferred_width=2945;hello.preferred_height=1841;
    for (const auto rate : {0u,166u,UINT32_MAX}) {
        hello.preferred_fps=rate;mode=pds::ResolveVideoRequest(Parse({}),hello);
        Check(mode.width==2944 && mode.height==1840 && mode.fps==60, "invalid rate fallback and even size");
    }
    hello.preferred_fps=15;
    Check(pds::ResolveVideoRequest(Parse({}),hello).fps==15,"existing low-rate clients remain valid");
    hello.preferred_width=2944;hello.preferred_height=1840;hello.decoder_max_width=1920;hello.decoder_max_height=1200;
    mode=pds::ResolveVideoRequest(Parse({}),hello);
    Check(mode.width==1920 && mode.height==1200, "actual decoder maximum bounds requested display mode");
    LOG_INFO("PASS stage 6: CLI defaults/switches, explicit > HELLO > default, validation, even dimensions and decoder bounds");
}
void FallbackTest() {
    const pds::VideoMode current{1920,1200,60}, request{2944,1840,144};
    std::vector<pds::VideoMode> attempts;
    auto fail=[&](const pds::VideoMode& mode)->std::optional<pds::VideoMode>{attempts.push_back(mode);return std::nullopt;};
    auto actual=pds::AdaptDisplayMode(current,request,true,fail);
    Check(attempts.size()==2 && attempts[0].fps==144 && attempts[1].fps==60, "target then 60 attempts");
    Check(actual.width==1920 && actual.height==1200 && actual.fps==60, "unavailable native resolution keeps current");
    attempts.clear();
    actual=pds::AdaptDisplayMode(current,request,true,[&](const pds::VideoMode& mode)->std::optional<pds::VideoMode>{
        attempts.push_back(mode);return mode.fps==60?std::optional(mode):std::nullopt;
    });
    Check(actual.width==2944 && actual.height==1840 && actual.fps==60 && attempts.size()==2, "144 unavailable keeps native size at 60");
    attempts.clear();
    actual=pds::AdaptDisplayMode(current,request,true,[&](const pds::VideoMode& mode)->std::optional<pds::VideoMode>{attempts.push_back(mode);return mode;});
    Check(actual.width==2944 && actual.fps==144 && attempts.size()==1, "native 144 succeeds once");
    attempts.clear();
    actual=pds::AdaptDisplayMode(current,request,false,fail);
    Check(attempts.empty() && actual.width==1920 && actual.fps==144, "no resize means no display API calls");
    actual=pds::AdaptDisplayMode(current,{2944,1840,30},true,[](const pds::VideoMode& mode)->std::optional<pds::VideoMode>{return mode.fps==60?std::optional(mode):std::nullopt;});
    Check(actual.fps==30, "60Hz mode fallback never raises encoding above cap");
    actual=pds::AdaptDisplayMode(current,request,true,[](const pds::VideoMode& mode)->std::optional<pds::VideoMode>{return pds::VideoMode{mode.width,mode.height,120};});
    Check(actual.fps==120, "read-back display rate bounds encoder/ACK");
    // Same-ratio fallback: exact 2944x1840@144 and @60 fail, driver offers only
    // smaller 16:10 modes, so the largest one is applied instead of a 16:9 mode.
    attempts.clear();
    const std::vector<pds::VideoMode> smaller16x10{{1280,800,60},{1920,1080,60},{2560,1600,60}};
    const auto apply2560=[&](const pds::VideoMode& mode)->std::optional<pds::VideoMode>{
        attempts.push_back(mode);return mode.width==2560?std::optional(mode):std::nullopt;
    };
    actual=pds::AdaptDisplayMode(current,request,true,apply2560,[&]{return smaller16x10;});
    Check(actual.width==2560 && actual.height==1600 && actual.fps==60 && attempts.size()==2,
        "largest same-ratio mode applied after exact failures");
    Check(attempts[0].fps==144 && attempts[1].width==2560 && attempts[1].fps==60, "exact attempt precedes ratio fallback");
    attempts.clear();
    actual=pds::AdaptDisplayMode(current,request,true,[&](const pds::VideoMode& mode)->std::optional<pds::VideoMode>{
        attempts.push_back(mode);return mode.fps==120?std::optional(mode):std::nullopt;
    },[&]{return std::vector<pds::VideoMode>{{2944,1840,120},{2560,1600,60}};});
    Check(actual.width==2944 && actual.height==1840 && actual.fps==120 && attempts.size()==2, "native size kept at a slower driver rate");
    attempts.clear();
    const std::vector<pds::VideoMode> broken_native{{2944,1840,144},{2560,1600,144}};
    const auto rejectNative=[&](const pds::VideoMode& mode)->std::optional<pds::VideoMode>{
        attempts.push_back(mode);return mode.width==2560?std::optional(mode):std::nullopt;
    };
    actual=pds::AdaptDisplayMode(current,request,true,rejectNative,[&]{return broken_native;});
    Check(actual.width==2560 && actual.height==1600 && actual.fps==144 && attempts.size()==2,
        "next same-ratio size tried when a listed native mode is rejected");
    attempts.clear();
    const std::vector<pds::VideoMode> native165{{2560,1600,144},{1280,800,60}};
    actual=pds::AdaptDisplayMode(current,{2560,1600,165},true,
        [&](const pds::VideoMode& mode)->std::optional<pds::VideoMode>{attempts.push_back(mode);return mode.fps==144?std::optional(mode):std::nullopt;},
        [&]{return native165;});
    Check(actual.fps==144 && attempts.size()==2 && attempts[1].fps==144, "native 144Hz preferred over the 60Hz retry for a 165Hz request");
    attempts.clear();
    actual=pds::AdaptDisplayMode(current,request,true,fail,[&]{return std::vector<pds::VideoMode>{{1920,1080,60},{2560,1440,120}};});
    Check(attempts.size()==2 && actual.width==1920, "16:9 modes never stand in for a 16:10 request");
    bool listed=false;
    attempts.clear();
    actual=pds::AdaptDisplayMode(current,request,false,fail,[&]{listed=true;return smaller16x10;});
    Check(!listed && attempts.empty(), "disabled resize never enumerates driver modes");
    LOG_INFO("PASS stage 6: injected mode application 144 -> 60 -> current, read-back rate and cap protection; same-ratio fallback; no display settings changed");
}
void SameRatioTest() {
    using pds::VideoMode;
    const std::vector<pds::VideoMode> modes{{1920,1080,60},{2560,1440,60},{3840,2160,60},
        {1280,800,60},{1600,1000,60},{1920,1200,60},{2560,1600,60},{2944,1840,120}};
    auto picked=pds::ChooseSameRatioMode(modes,{2944,1840,144});
    Check(picked && picked->width==2944 && picked->height==1840 && picked->fps==120, "exact size with slower refresh than requested");
    const auto ranked=pds::RankSameRatioModes(modes,{2944,1840,144});
    Check(ranked.size()>=3 && ranked[0].width==2944 && ranked[1].width==2560 && ranked[2].width==1920,
        "ranking orders native then largest-to-smallest same-ratio sizes");
    picked=pds::ChooseSameRatioMode(modes,{3200,2000,60});
    Check(picked && picked->width==2944 && picked->height==1840, "largest same-ratio mode not exceeding the request");
    const std::vector<pds::VideoMode> below_only{{1280,800,60},{3840,2400,60}};
    picked=pds::ChooseSameRatioMode(below_only,{2944,1840,144});
    Check(picked && picked->width==1280 && picked->height==800, "below-only candidates prefer the largest");
    const std::vector<pds::VideoMode> above_only{{3840,2400,60},{4096,2560,60}};
    picked=pds::ChooseSameRatioMode(above_only,{2944,1840,144});
    Check(picked && picked->width==3840 && picked->height==2400, "above-only candidates prefer the smallest");
    const std::vector<pds::VideoMode> slow_native{{2944,1840,30},{2560,1600,144}};
    picked=pds::ChooseSameRatioMode(slow_native,{2944,1840,144});
    Check(picked && picked->width==2560 && picked->height==1600 && picked->fps==144, "60Hz+ mode beats native size stuck below 60Hz");
    const std::vector<pds::VideoMode> only_slow_native{{2944,1840,30}};
    picked=pds::ChooseSameRatioMode(only_slow_native,{2944,1840,144});
    Check(picked && picked->width==2944 && picked->height==1840 && picked->fps==30, "only slow mode is still usable");
    const std::vector<pds::VideoMode> wide_only{{1920,1080,60},{2560,1440,60}};
    Check(!pds::ChooseSameRatioMode(wide_only,{2944,1840,144}), "16:9 modes are rejected for a 16:10 request");
    const std::vector<pds::VideoMode> rate_list{{1920,1200,30},{1920,1200,90},{1920,1200,120},{1920,1200,165}};
    picked=pds::ChooseSameRatioMode(rate_list,{2944,1840,100});
    Check(picked && picked->fps==90, "fastest refresh not above the request");
    const std::vector<pds::VideoMode> slow_list{{1920,1200,30},{1920,1200,60}};
    picked=pds::ChooseSameRatioMode(slow_list,{2944,1840,20});
    Check(picked && picked->fps==30, "slowest refresh when every mode exceeds the request");
    Check(!pds::ChooseSameRatioMode({},{2944,1840,144}), "empty mode list has no fallback");
    Check(!pds::ChooseSameRatioMode(modes,{0,0,144}), "invalid request has no fallback");
    LOG_INFO("PASS stage 6: same-ratio mode choice (exact, nearest size, refresh preference, ratio rejection)");
}
void CadenceTest() {
    for (const auto hz : {90u,120u,144u,165u}) {
        pds::FramePacer pacer(true,hz);unsigned count=0;
        for(uint64_t frame=0;frame<static_cast<uint64_t>(hz)*10;++frame) if(pacer.Accept(frame*1000000/hz))++count;
        Check(count==hz*10, "high rate accepted without changing cadence policy");
        pds::WgcCapture capture(true);Check(capture.ConfigureThrottle(true,hz), "WGC accepts negotiated high frame rate");
    }
    bool rejected=false;try{pds::FramePacer bad(true,166);}catch(const std::invalid_argument&){rejected=true;}
    Check(rejected,"pacer refuses rates above 165");
    LOG_INFO("PASS stage 6: 90/120/144/165 FPS pacing policy (synthetic callback times, not hardware performance)");
}
void ConfigTest() {
    const auto path=std::filesystem::temp_directory_path()/(L"SecondaryScreen-stage6-"+std::to_wstring(GetCurrentProcessId())+L".json");
    struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code error;std::filesystem::remove(path,error);}} cleanup{path};
    pds::AppConfig value;value.max_fps=60;
    Check(pds::SaveConfig(path,value)&&pds::LoadConfig(path)==value,"v2 adaptive config roundtrip");
    auto options=pds::SessionFromConfig(value);
    Check(!options.width_explicit&&!options.height_explicit&&options.fps==0&&options.max_fps==60&&options.auto_resize,"GUI defaults follow tablet");
    {std::ofstream file(path);file<<R"({"version":1,"display":{"width":1920,"height":1200,"fps":60,"autoResize":false},"encoder":{"bitrateKbps":10000},"connection":{"port":51900},"app":{}})";}
    value=pds::LoadConfig(path);options=pds::SessionFromConfig(value);
    Check(!value.automatic_size&&options.width_explicit&&options.height_explicit&&options.fps==60&&!options.auto_resize,"v1 preserves manual preferences");
    Check(pds::SaveConfig(path,value)&&pds::LoadConfig(path)==value,"legacy migrates on save");
    LOG_INFO("PASS stage 6: GUI config v2 adaptive defaults and v1 manual settings migration");
}
void SocketTest(uint32_t explicit_fps,uint32_t cap,uint16_t expected) {
    pds::TcpServer server;Check(server.Listen(0),"loopback listen");
    sockaddr_in address{};int length=sizeof(address);
    Check(getsockname(server.Socket(),reinterpret_cast<sockaddr*>(&address),&length)==0,"ephemeral port");
    pds::UniqueSocket client(socket(AF_INET,SOCK_STREAM,IPPROTO_TCP));
    DWORD timeout=5000;setsockopt(client.Get(),SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
    Check(connect(client.Get(),reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"loopback connect");
    std::string peer;pds::UniqueSocket accepted(server.Accept(peer));
    pds::SessionOptions options;options.capture_primary=true;options.auto_resize=false;options.disable_hardware=true;
    options.width=320;options.height=200;options.width_explicit=true;options.height_explicit=true;options.fps=explicit_fps;options.max_fps=cap;
    pds::PipelineSession session(std::move(accepted),options);session.Run();
    auto header=pds::Serialize(pds::MakeHeader(pds::MessageType::Hello,sizeof(pds::Hello)));
    auto hello=pds::Serialize(pds::Hello{1,0,2944,1840,144,320,4096,4096,3});
    Check(pds::TcpServer::SendAll(client.Get(),header.data(),header.size())&&pds::TcpServer::SendAll(client.Get(),hello.data(),hello.size()),"send native HELLO");
    pds::MessageHeader received{};pds::HelloAck ack{};
    Check(pds::TcpServer::RecvExact(client.Get(),&received,sizeof(received))&&pds::ValidHeader(received)&&received.length==sizeof(ack),"ACK header contract");
    Check(pds::TcpServer::RecvExact(client.Get(),&ack,sizeof(ack))&&ack.result==0&&ack.codec==1&&ack.width==320&&ack.height==200&&ack.fps==expected&&ack.frame_interval_us==1000000u/expected,"actual ACK has manual size and capped fallback rate");
    Check(pds::TcpServer::RecvExact(client.Get(),&received,sizeof(received))&&received.type==static_cast<uint16_t>(pds::MessageType::VideoFrame),"real JPEG data follows ACK");
    client.Reset();
    LOG_INFO("PASS stage 6 TCP: native HELLO 2944x1840@144 -> manual 320x200 JPEG fps=",ack.fps," (real fallback; display untouched)");
}
}
int main() {
    try {
        RequestTest();FallbackTest();SameRatioTest();CadenceTest();ConfigTest();
        pds::WinsockRuntime winsock;Check(winsock.Ready(),"Winsock ready");
        SocketTest(0,0,15);SocketTest(144,60,60);SocketTest(0,5,5);
        return 0;
    }catch(const std::exception& error){LOG_ERROR("FAIL stage 6: ",error.what());return 1;}
}
