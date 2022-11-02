#include "cpucounters.h"
#include "pcm-pcie.h"
#include "cxxopts.hpp"
//pcm-raw -e imc/config=0x09,name=ECC_CORRECTABLE_ERRORS/
//https://github.com/Chester-Gillon/pcm

using namespace std;
using namespace pcm;

bool DEBUG=false;
bool SHOW_CHANNELS=false;
bool SHOW_MEMORY=true;
bool SHOW_PCIE=true;
constexpr uint32 max_sockets = 256;
uint32 max_imc_channels = ServerUncoreCounterState::maxChannels;
const uint32 max_edc_channels = ServerUncoreCounterState::maxChannels;
const uint32 max_imc_controllers = ServerUncoreCounterState::maxControllers;

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

void printSocketBWFooter(uint32 numSockets, const ServerUncoreCounterState uncState1[], const ServerUncoreCounterState uncState2[], const uint64 elapsedTime){
    auto toBW = [&elapsedTime](const uint64 nEvents){
        return (float)(nEvents * 64 / 1000000.0 / (elapsedTime / 1000.0));
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
	            cout << "|-- NODE" << i << " channel " << channel << " read: " << setw(8) << toBW(reads) << " --|";
	            cout << "|-- NODE" << i << " channel " << channel << " writes: " << setw(8) << toBW(writes) << endl;
			}
        }
		if (SHOW_MEMORY){
            cout << "|-- NODE" << i << " Mem Read (MB/s) : " << setw(8) << toBW(sktReads)  << " --|";
            cout << "|-- NODE" << i << " Mem Write(MB/s) : " << setw(8) << toBW(sktWrites) << " --|" << endl;
	    }
    }
    cout << "\n";
}

int main(int argc, char** argv) {
    cxxopts::Options options("pmt", "cpu performance monitor tool");
    options.add_options()
        ("g,debug",   "Enable debug info",    cxxopts::value<bool>()->default_value("false"))
        ("v,version", "Version output",       cxxopts::value<bool>()->default_value("false"))
        ("c,channels","Show memory channels", cxxopts::value<bool>()->default_value("false"))
        ("m,memory",  "Show memory bandwidth",cxxopts::value<bool>()->default_value("true"))
        ("p,pcie",    "Show pcie bandwidth",  cxxopts::value<bool>()->default_value("true"))
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
	/////////////////////////////////////////////
    PCM *m = PCM::getInstance();
    PCM::ErrorCode returnResult = m->program();
    if (returnResult != PCM::Success) {
        std::cerr << "PCM couldn't start" << std::endl;
        std::cerr << "Error code: " << returnResult << std::endl;
        exit(1);
    }
    unique_ptr<IPlatform> platform(IPlatform::getPlatform(m, false, true, true, (uint)2));
    if (platform == NULL){
        std::cout << "unsupported platform, exiting." << std::endl;
        return -1;
    }
    uint32 numSockets = m->getNumSockets();
    max_imc_channels = (pcm::uint32)m->getMCChannelsPerSocket();
    ServerUncoreCounterState * BeforeState = new ServerUncoreCounterState[m->getNumSockets()];
    ServerUncoreCounterState * AfterState = new ServerUncoreCounterState[m->getNumSockets()];
    uint64 BeforeTime = 0, AfterTime = 0;
    BeforeTime = m->getTickCount();
    for(;;){
        //std::cout << "=====================================" << NUM_SAMPLES << std::endl;
        for (uint32 i=0; i<numSockets; ++i) {
            AfterState[i] = m->getServerUncoreCounterState(i);
        }
        AfterTime = m->getTickCount();
        platform->getEvents();
		if (SHOW_PCIE){
            platform->printHeader();
            platform->printEvents();
		}
        printSocketBWFooter(numSockets,BeforeState,AfterState,AfterTime-BeforeTime);
        swap(BeforeTime, AfterTime);
        swap(BeforeState, AfterState);
    }
    delete[] BeforeState;
    delete[] AfterState;
    //std::cout << "=====================================" << std::endl;
    //SystemCounterState before_sstate = getSystemCounterState();
    //SystemCounterState after_sstate = getSystemCounterState();

    //std::cout << "Instructions per clock:" << getIPC(before_sstate, after_sstate) << std::endl;
    //std::cout << "Bytes read:" << getBytesReadFromMC(before_sstate, after_sstate) << std::endl;
    m->cleanup();
}

