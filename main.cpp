#include "cpucounters.h"
#include "cxxopts.hpp"
#include <iostream>
#include <string>
#include <stdio.h>
#include <time.h>
#include <chrono>
#include <map>
#include <math.h>
#include "pcie.h"
//pcm-raw -e imc/config=0x09,name=ECC_CORRECTABLE_ERRORS/
//https://github.com/Chester-Gillon/pcm

using namespace std;
using namespace pcm;

float delay=1.0;
bool DEBUG=false;
bool SHOW_CHANNELS=false;
bool SHOW_MEMORY=false;
bool SHOW_PCIE=false;
string SEP="    "; //\t";
constexpr uint32 max_sockets = 256;
uint32 max_imc_channels = ServerUncoreCounterState::maxChannels;
const uint32 max_edc_channels = ServerUncoreCounterState::maxChannels;
const uint32 max_imc_controllers = ServerUncoreCounterState::maxControllers;

const std::string currentDateTime() {
    tm localTime;
    std::chrono::system_clock::time_point t = std::chrono::system_clock::now();
    time_t now = std::chrono::system_clock::to_time_t(t);
    localtime_r(&now, &localTime);
    const std::chrono::duration<double> tse = t.time_since_epoch();
    std::chrono::seconds::rep milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(tse).count() % 1000;
    char buf[32];
    if (delay<1){
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", localTime.tm_hour, localTime.tm_min, localTime.tm_sec, milliseconds);
    }else{
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", localTime.tm_hour, localTime.tm_min, localTime.tm_sec);
    }
    return buf;
}

IPlatform *IPlatform::getPlatform(PCM *m, bool csv, bool bw, bool verbose, uint32 delay){
    switch (m->getCPUModel()) {
        case PCM::ICX:
        case PCM::SNOWRIDGE:
            return new WhitleyPlatform(m, csv, bw, verbose, delay);
        case PCM::SKX:
            return new PurleyPlatform(m, csv, bw, verbose, delay);
        case PCM::BDX_DE:
        case PCM::BDX:
        case PCM::KNL:
        case PCM::HASWELLX:
            return new GrantleyPlatform(m, csv, bw, verbose, delay);
        case PCM::IVYTOWN:
        case PCM::JAKETOWN:
            return new BromolowPlatform(m, csv, bw, verbose, delay);
        default:
          return NULL;
    }
}

void printMemBW(uint32 numSockets, const ServerUncoreCounterState uncState1[], const ServerUncoreCounterState uncState2[], const uint64 elapsedTime){
    auto toBW = [&elapsedTime](const uint64 nEvents){
        float val=(nEvents * 64 / 1000000.0 / (elapsedTime / 1000.0));
        return roundf(val * 100) / 100;
    };
    uint64 reads=0, writes=0;
    for (uint32 i=0; i<numSockets; ++i) {
        uint64 sktReads=0, sktWrites=0;
        for (uint32 channel=0; channel<max_imc_channels; ++channel){
            reads  = getMCCounter(channel, ServerPCICFGUncore::EventPosition::READ,  uncState1[i], uncState2[i]);
            writes = getMCCounter(channel, ServerPCICFGUncore::EventPosition::WRITE, uncState1[i], uncState2[i]);
            sktReads+=reads;
            sktWrites+=writes;
			if (SHOW_CHANNELS){
	            cout << SEP << setw(6) << toBW(reads) << SEP << setw(6) << toBW(writes);
			}
        }
		if (SHOW_MEMORY){
            cout << SEP << setw(6) << toBW(sktReads) << SEP << setw(6) << toBW(sktWrites);
	    }
    }
    cout << "\n";
}

int main(int argc, char** argv) {
    cxxopts::Options options("pmt", "cpu performance monitor tool");
    options.add_options()
        ("g,debug",   "Enable debug info",    cxxopts::value<bool>()->default_value("false"))
        ("v,version", "Version output",       cxxopts::value<bool>()->default_value("false"))
        ("s,delay",   "Seconds/update",       cxxopts::value<float>()->default_value("1.0"))
        ("m,memory",  "Show memory bandwidth",cxxopts::value<bool>()->default_value("true"))
        ("c,channels","Show memory channels", cxxopts::value<bool>()->default_value("false"))
        ("p,pcie",    "Show pcie bandwidth",  cxxopts::value<bool>()->default_value("false"))
        ("h,help",    "Print usage")
        //("n,duration","Duration",         cxxopts::value<int>()->default_value("60"))
    ;
    auto result = options.parse(argc, argv);
    if (result.count("help")){
      std::cout << options.help() << std::endl;
      exit(0);
    }
    if (result.count("version")){
      std::cout << "cpu performance monitor tool\n" << "version: 0.0.1" << std::endl;
      exit(0);
    }
    DEBUG = result["debug"].as<bool>();
    //if (DEBUG) spdlog::set_level(spdlog::level::debug);
    //else spdlog::set_level(spdlog::level::warn);
    SHOW_CHANNELS=result["channels"].as<bool>();
    SHOW_MEMORY=result["memory"].as<bool>();
    SHOW_PCIE=result["pcie"].as<bool>();
    delay=result["delay"].as<float>(); //PCM_DELAY_DEFAULT
    /////////////////////////////////////////////
    PCM *m = PCM::getInstance();
    PCM::ErrorCode returnResult = m->program();
    if (returnResult != PCM::Success) {
        std::cerr << "PCM couldn't start" << std::endl;
        std::cerr << "Error code: " << returnResult << std::endl;
        exit(1);
    }
    unique_ptr<IPlatform> platform(IPlatform::getPlatform(m, false, true, true, (uint)delay));
    if (platform == NULL){
        std::cout << "unsupported platform, exiting." << std::endl;
        return -1;
    }else{
        std::cout << m->getCPUModel() << " CPU Detected." << std::endl;
    }
    uint32 numSockets = m->getNumSockets();
    max_imc_channels = (pcm::uint32)m->getMCChannelsPerSocket();
    cout << "Time      ";
    if (SHOW_CHANNELS){
        for (uint32 i=0; i<numSockets; ++i) {
            for (uint32 c=0; c<max_imc_channels; ++c){
                cout <<SEP<< "S"<<i<<"C"<<c<<"R" <<SEP<< "S"<<i<<"C"<<c<<"W" ;
            }
        }
    }
    if (SHOW_MEMORY){
        for (uint32 i=0; i<numSockets; ++i) {
            cout <<SEP<< "S"<<i<<"Read" <<SEP<< "S"<<i<<"Write" ;
        }
    }
    cout << endl;

    ServerUncoreCounterState * BeforeState = new ServerUncoreCounterState[m->getNumSockets()];
    ServerUncoreCounterState * AfterState = new ServerUncoreCounterState[m->getNumSockets()];
    uint64 BeforeTime = 0, AfterTime = 0;
    BeforeTime = m->getTickCount();
    for (;;){
        cout << currentDateTime();
        if (SHOW_PCIE){
            platform->getEvents();//pcie
            platform->printHeader();
            platform->printEvents();
        }
        for (uint32 i=0; i<numSockets; ++i) {
            AfterState[i] = m->getServerUncoreCounterState(i);
        }
        AfterTime = m->getTickCount();
        printMemBW(numSockets,BeforeState,AfterState,AfterTime-BeforeTime);
        swap(BeforeTime, AfterTime);
        swap(BeforeState, AfterState);
        platform->cleanup();
        MySleepMs(delay*1000);
    }
    //    if (m->isBlocked())
    //        return false;
    //    return true;
    //});
    delete[] BeforeState;
    delete[] AfterState;
    //std::cout << "=====================================" << std::endl;
    //SystemCounterState before_sstate = getSystemCounterState();
    //SystemCounterState after_sstate = getSystemCounterState();

    //std::cout << "Instructions per clock:" << getIPC(before_sstate, after_sstate) << std::endl;
    //std::cout << "Bytes read:" << getBytesReadFromMC(before_sstate, after_sstate) << std::endl;
    m->cleanup();
    exit(EXIT_SUCCESS);
}

