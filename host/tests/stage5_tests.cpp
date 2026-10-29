#include "adb/adb_manager.h"
#include "app/config.h"
#include "util/log.h"
#include <fstream>
#include <stdexcept>
#include <thread>
namespace {
void Check(bool value,const char* text){if(!value)throw std::runtime_error(text);}
void ConfigTest(const std::filesystem::path& directory) {
    const auto path=directory/L"config.json";
    Check(pds::LoadConfig(path)==pds::AppConfig{},"missing config defaults");
    pds::AppConfig value;value.width=1280;value.height=800;value.device_serial="SERIAL-1";value.foreground=true;
    Check(pds::SaveConfig(path,value)&&pds::LoadConfig(path)==value,"config roundtrip");
    value.port=52141;Check(pds::SaveConfig(path,value)&&pds::LoadConfig(path)==value,"atomic existing config replace");
    auto temporary=path;temporary+=L".tmp";Check(!std::filesystem::exists(temporary),"no tmp after replace");
    {std::ofstream file(path);file<<"{broken";}Check(pds::LoadConfig(path)==pds::AppConfig{},"corrupt config defaults");
    Check(pds::SaveConfig(path,value),"restore fixture");
    std::string text;{std::ifstream file(path);text.assign(std::istreambuf_iterator<char>(file),{});}
    const auto pos=text.find("1280");Check(pos!=std::string::npos,"width in saved config");text.replace(pos,4,"4294968576");
    {std::ofstream file(path);file<<text;}Check(pds::LoadConfig(path)==pds::AppConfig{},"oversized integer cannot wrap into valid width");
    LOG_INFO("PASS stage 5: missing/corrupt/range config, roundtrip and atomic replacement");
}
void AdbTest() {
    const auto devices=pds::ParseAdbDevices("* daemon started successfully\nList of devices attached\nA device model:Tablet_A transport_id:1\nB unauthorized\nC offline\n");
    Check(devices.size()==3&&devices[0].model=="Tablet_A"&&devices[1].state=="unauthorized","devices parser");
    Check(pds::HasReverse("UsbFfs tcp:51900 tcp:51900\n",51900)&&!pds::HasReverse("UsbFfs tcp:51900 tcp:51901\n",51900),"exact reverse endpoints");
    std::string listing="List of devices attached\nA unauthorized\n";bool mapped=false;unsigned creates=0,removes=0,launches=0;bool fail_reverse=false;
    auto runner=[&](const std::vector<std::wstring>& args){
        pds::ProcessResult result;result.exit_code=0;
        if(args.front()==L"devices")result.output=listing;
        if(args.size()>3&&args[2]==L"reverse") {
            if(args[3]==L"--list")result.output=mapped?"UsbFfs tcp:51900 tcp:51900\n":"";
            else if(args[3]==L"--remove"){mapped=false;++removes;}
            else {++creates;if(fail_reverse)result.exit_code=1;else mapped=true;}
        }
        if(args.size()>2&&args[2]==L"shell")++launches;
        return result;
    };
    {
        pds::AdbManager manager(L"fixture-adb.exe",runner);
        auto state=manager.Poll(51900,"",true,false);Check(!state.ready&&state.message.find("允许")!=std::string::npos&&creates==0,"unauthorized prompt without reverse");
        listing="List of devices attached\nA device model:Tablet_A\nB device model:Tablet_B\n";
        state=manager.Poll(51900,"",true,false);Check(!state.ready&&creates==0,"multiple devices require selection");
        state=manager.Poll(51900,"B",true,false);Check(state.ready&&state.serial=="B"&&creates==1&&launches==1,"remembered device and automatic reverse/launch");
        state=manager.Poll(51900,"B",true,false);Check(state.ready&&!state.changed&&creates==1&&launches==1,"existing mapping preserved without relaunch");
        mapped=false;state=manager.Poll(51900,"B",true,false);Check(state.ready&&state.changed&&creates==2&&launches==2,"mapping disappearance repaired");
        listing="List of devices attached\n";state=manager.Poll(51900,"B",true,false);Check(!state.ready&&removes==1,"USB removal stops readiness and releases owned mapping");
        listing="List of devices attached\nB device\n";state=manager.Poll(51900,"B",true,false);Check(state.ready&&creates==3,"USB replug recovers");
        manager.Reset();Check(removes==2,"stop removes only owned mapping");
        fail_reverse=true;state=manager.Poll(51900,"B",true,false);Check(!state.ready&&state.error,"reverse failure is recoverable error");
        fail_reverse=false;state=manager.Poll(51900,"B",true,false);Check(state.ready,"retry after single command failure");
    }
    mapped=true;const auto prior_removes=removes;
    {pds::AdbManager manager(L"fixture-adb.exe",runner);Check(manager.Poll(51900,"B",true,false).ready,"adopt existing correct mapping");}
    Check(removes==prior_removes,"existing mapping not removed by destructor");
    LOG_INFO("PASS stage 5: adb authorization, multi-device selection, mapping repair, replug and ownership (injected runner)");
}
void ProcessTest(const std::filesystem::path& self) {
    auto result=pds::RunProcess(self,{L"--fixture",L"",L"space and 中文",L"quote\"slash\\"});
    Check(result.exit_code==0&&!result.timed_out&&result.output.find("fixture arguments exact and no console")!=std::string::npos,"real hidden process and Windows argv quoting");
    result=pds::RunProcess(self,{L"--fixture-wait"},std::chrono::milliseconds(80));Check(result.timed_out&&result.exit_code==ERROR_TIMEOUT,"bounded child timeout and termination");
    result=pds::RunProcess(self,{L"--fixture-output"});Check(result.exit_code==0&&result.output.size()==128*1024,"pipe drains output exceeding pipe capacity");
    result=pds::RunProcess(self,{L"--fixture-output-loop"},std::chrono::milliseconds(80));Check(result.timed_out&&result.output.size()<=1024*1024,"continuous output cannot starve timeout or grow diagnostics unbounded");
    LOG_INFO("PASS stage 5: real CREATE_NO_WINDOW child, unicode/empty/quoted argv, output drain and timeout");
}
}
int wmain(int argc,wchar_t** argv) {
    if(pds::ExecutableDirectory().filename()==L"platform-tools" && std::filesystem::path(argv[0]).filename()==L"adb.exe") {
        // Isolated integration fixture only: mock USB readiness, never video or codec.
        // This test executable is never shipped in the release payload.
        std::string reply;
        if(argc>1&&std::wstring_view(argv[1])==L"devices")reply="List of devices attached\nSECONDARYSCREEN_TEST_ONLY device model:Simulated_USB\n";
        for(int i=1;i<argc;++i)if(std::wstring_view(argv[i])==L"--list")reply="TestTransport tcp:52150 tcp:52150\n";
        DWORD count=0;return WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),reply.data(),static_cast<DWORD>(reply.size()),&count,nullptr)?0:1;
    }
    if(argc>1&&std::wstring_view(argv[1])==L"--fixture") {
        if(argc!=5||std::wstring_view(argv[2])!=L""||std::wstring_view(argv[3])!=L"space and 中文"||std::wstring_view(argv[4])!=L"quote\"slash\\"||GetConsoleWindow())return 1;
        LOG_INFO("fixture arguments exact and no console");return 0;
    }
    if(argc>1&&std::wstring_view(argv[1])==L"--fixture-wait"){std::this_thread::sleep_for(std::chrono::seconds(5));return 0;}
    if(argc>1&&(std::wstring_view(argv[1])==L"--fixture-output"||std::wstring_view(argv[1])==L"--fixture-output-loop")) {
        // Raw pipe fixture, not an application log.
        const bool forever=std::wstring_view(argv[1])==L"--fixture-output-loop";
        const std::string bytes(4096,'x');for(int i=0;forever||i<32;++i){DWORD count=0;if(!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr)||count!=bytes.size())return 1;if(forever)i=0;}return 0;
    }
    try {
        wchar_t temporary[MAX_PATH]{};Check(GetTempPathW(MAX_PATH,temporary)>0,"temp directory");
        const auto directory=std::filesystem::path(temporary)/(L"SecondaryScreen-stage5-"+std::to_wstring(GetCurrentProcessId()));
        Check(std::filesystem::create_directory(directory),"fresh isolated fixture directory");
        struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code error;std::filesystem::remove_all(path,error);}} cleanup{directory};
        ConfigTest(directory);AdbTest();ProcessTest(pds::ExecutableDirectory()/L"secondaryscreen-stage5-tests.exe");return 0;
    }catch(const std::exception& error){LOG_ERROR("FAIL stage 5: ",error.what());return 1;}
}
