#include "cpucounters.h"
#include <unistd.h>
#include <memory>
#include <fstream>
#include <sstream>
#include <stdlib.h>
#include <stdexcept>
#include <cstdint>
#include <numeric>
#include <algorithm>
#include "lspci.h"
#include "utils.h"
#include "cxxopts.hpp"
using namespace std;
using namespace pcm;

string csv_delimiter = ",";
std::ostream* OUT = &std::cout;
string OUT_FILE="";
vector<string> ONLY;
float delay=1.0;
bool DEBUG=false;
const uint8_t max_sockets = 4;
static const std::string iio_stack_names[6] = {
    "IIO Stack 0 - CBDMA/DMI      ",
    "IIO Stack 1 - PCIe0          ",
    "IIO Stack 2 - PCIe1          ",
    "IIO Stack 3 - PCIe2          ",
    "IIO Stack 4 - MCP0           ",
    "IIO Stack 5 - MCP1           "
};
static const std::string icx_iio_stack_names[6] = {
    "IIO Stack 0 - PCIe0          ",
    "IIO Stack 1 - PCIe1          ",
    "IIO Stack 2 - MCP            ",
    "IIO Stack 3 - PCIe2          ",
    "IIO Stack 4 - PCIe3          ",
    "IIO Stack 5 - CBDMA/DMI      "
};
static const std::string icx_d_iio_stack_names[6] = {
    "IIO Stack 0 - MCP            ",
    "IIO Stack 1 - PCIe0          ",
    "IIO Stack 2 - CBDMA/DMI      ",
    "IIO Stack 3 - PCIe2          ",
    "IIO Stack 4 - PCIe3          ",
    "IIO Stack 5 - PCIe1          "
};

#define ICX_CBDMA_DMI_SAD_ID 0
#define ICX_MCP_SAD_ID       3
#define ICX_PCH_PART_ID   0
#define ICX_CBDMA_PART_ID 3
#define SNR_ICX_SAD_CONTROL_CFG_OFFSET 0x3F4
#define SNR_ICX_MESH2IIO_MMAP_DID      0x09A2
#define ICX_VMD_PCI_DEVNO   0x00
#define ICX_VMD_PCI_FUNCNO  0x05
static const std::map<int, int> icx_sad_to_pmu_id_mapping = {
    { ICX_CBDMA_DMI_SAD_ID, 5 },
    { 1,                    0 },
    { 2,                    1 },
    { ICX_MCP_SAD_ID,       2 },
    { 4,                    3 },
    { 5,                    4 }
};
static const std::map<int, int> icx_d_sad_to_pmu_id_mapping = {
    { ICX_CBDMA_DMI_SAD_ID, 2 },
    { 1,                    5 },
    { 2,                    1 },
    { ICX_MCP_SAD_ID,       0 },
    { 4,                    3 },
    { 5,                    4 }
};

map<string,PCM::PerfmonField> opcodeFieldMap;
map<string,std::pair<h_id,std::map<string,v_id>>> nameMap;
result_content results(max_sockets, stack_content(6, ctr_data()));

struct data{
    uint32_t width;
    uint64_t value;
};

void print_nameMap() {
    for (std::map<string,std::pair<h_id,std::map<string,v_id>>>::const_iterator iunit = nameMap.begin(); iunit != nameMap.end(); ++iunit)
    {
        string h_name = iunit->first;
        std::pair<h_id,std::map<string,v_id>> value = iunit->second;
        uint32_t hid = value.first;
        std::map<string,v_id> vMap = value.second;
        cout << "H name: " << h_name << " id =" << hid << " vMap size:" << vMap.size() << "\n";
        for (std::map<string,v_id>::const_iterator junit = vMap.begin(); junit != vMap.end(); ++junit)
        {
            string v_name = junit->first;
            uint32_t vid = junit->second;
            cout << "V name: " << v_name << " id =" << vid << "\n";

        }
    }
}

string a_title (const string &init, const string &name) {
    char begin = init[0];
    string row = init;
    row += name;
    return row + begin;
}

string a_data (string init, struct data d) {
    char begin = init[0];
    string row = init;
    string str_d = unit_format(d.value);
    row += str_d;
    if (str_d.size() > d.width)
        throw std::length_error("counter value > event_name length");
    row += string(d.width - str_d.size(), ' ');
    return row + begin;
}

string build_line(string init, string name, bool last_char = true, char this_char = '_'){
    char begin = init[0];
    string row = init;
    row += string(name.size(), this_char);
    if (last_char == true)
        row += begin;
    return row;
}

string a_header_footer (string init, string name){
    return build_line(init, name);
}

vector<string> combine_stack_name_and_counter_names(string stack_name){
    vector<string> v;
    vector<string> tmp(nameMap.size());
    v.push_back(stack_name);
    for (std::map<string,std::pair<h_id,std::map<string,v_id>>>::const_iterator iunit = nameMap.begin(); iunit != nameMap.end(); ++iunit) {
        string h_name = iunit->first;
        int h_id = (iunit->second).first;
        tmp[h_id] = h_name;
        //cout << "h_id:" << h_id << " name:" << h_name << "\n";
    }
    //XXX: How to simplify and just combine tmp & v?
    for (uint32_t i = 0; i < nameMap.size(); i++) {
        v.push_back(tmp[i]);
    }
    return v;
}

vector<struct data> prepare_data(const vector<uint64_t> &values, const vector<string> &headers){
    vector<struct data> v;
    uint32_t idx = 0;
    for (std::vector<string>::const_iterator iunit = std::next(headers.begin()); iunit != headers.end() && idx < values.size(); ++iunit, idx++)
    {
        struct data d;
        d.width = (uint32_t)iunit->size();
        d.value = values[idx];
        v.push_back(d);
    }

    return v;
}

string build_pci_header(const PCIDB & pciDB, uint32_t column_width, struct pci p, int part = -1, uint32_t level = 0){
    string s = "|";
    char bdf_buf[10];
    char speed_buf[10];
    char vid_did_buf[10];
    char device_name_buf[128];

    snprintf(bdf_buf, sizeof(bdf_buf), "%02X:%02X.%1d", p.bdf.busno, p.bdf.devno, p.bdf.funcno);
    snprintf(speed_buf, sizeof(speed_buf), "Gen%1d x%-2d", p.link_speed, p.link_width);
    snprintf(vid_did_buf, sizeof(vid_did_buf), "%04X:%04X", p.vendor_id, p.device_id);
    snprintf(device_name_buf, sizeof(device_name_buf), "%s %s",
            (pciDB.first.count(p.vendor_id) > 0)?pciDB.first.at(p.vendor_id).c_str():"unknown vendor",
            (pciDB.second.count(p.vendor_id) > 0 && pciDB.second.at(p.vendor_id).count(p.device_id) > 0)?pciDB.second.at(p.vendor_id).at(p.device_id).c_str():"unknown device"
        );
    s += bdf_buf;
    s += '|';
    s += speed_buf;
    s += '|';
    s += vid_did_buf;
    s += " ";
    s += device_name_buf;

    /* row with data */
    if (part >= 0) {
        s.insert(1,"P" + std::to_string(part) + " ");
        s += std::string(column_width - (s.size()-1), ' ');
    } else { /* row without data, just child pci device */
        s.insert(0, std::string(4*level, ' '));
    }


    return s;
}

vector<string> build_display(vector<struct iio_stacks_on_socket>& iios, vector<struct counter>& ctrs, const PCIDB& pciDB){
    vector<string> buffer;
    vector<string> headers;
    vector<struct data> data;
    uint64_t header_width;
    string row;
    for (auto socket = iios.cbegin(); socket != iios.cend(); ++socket) {
        buffer.push_back("Socket" + std::to_string(socket->socket_id));
        for (auto stack = socket->stacks.cbegin(); stack != socket->stacks.cend(); ++stack) {
            auto stack_id = stack->iio_unit_id;
            headers = combine_stack_name_and_counter_names(stack->stack_name);
            //Print first row
            row = std::accumulate(headers.begin(), headers.end(), string(" "), a_header_footer);
            header_width = row.size();
            buffer.push_back(row);
            //Print a_title
            row = std::accumulate(headers.begin(), headers.end(), string("|"), a_title);
            buffer.push_back(row);
            //Print deliminator
            row = std::accumulate(headers.begin(), headers.end(), string("|"), a_header_footer);
            buffer.push_back(row);
            //Print data
            std::map<uint32_t,map<uint32_t,struct counter*>> v_sort;
            //re-organize data collection to be row wise
            for (std::vector<struct counter>::iterator counter = ctrs.begin(); counter != ctrs.end(); ++counter) {
                v_sort[counter->v_id][counter->h_id] = &(*counter);
            }
            for (std::map<uint32_t,map<uint32_t,struct counter*>>::const_iterator vunit = v_sort.cbegin(); vunit != v_sort.cend(); ++vunit) {
                map<uint32_t, struct counter*> h_array = vunit->second;
                uint32_t vv_id = vunit->first;
                vector<uint64_t> h_data;
                string v_name = h_array[0]->v_event_name;
                for (map<uint32_t,struct counter*>::const_iterator hunit = h_array.cbegin(); hunit != h_array.cend(); ++hunit) {
                    uint32_t hh_id = hunit->first;
                    uint64_t raw_data = hunit->second->data[0][socket->socket_id][stack_id][std::pair<h_id,v_id>(hh_id,vv_id)];
                    h_data.push_back(raw_data);
                }
                data = prepare_data(h_data, headers);
                row = "| " + v_name;
                row += string(headers[0].size() - (row.size() - 1), ' ');
                row += std::accumulate(data.begin(), data.end(), string("|"), a_data);
                buffer.push_back(row);
            }
            //Print deliminator
            row = std::accumulate(headers.begin(), headers.end(), string("|"), a_header_footer);
            buffer.push_back(row);
            //Print pcie devices
            for (const auto& part : stack->parts) {
                uint8_t level = 1;
                for (const auto& pci_device : part.child_pci_devs) {
                    row = build_pci_header(pciDB, (uint32_t)header_width, pci_device, -1, level);
                    buffer.push_back(row);
                    if (pci_device.header_type == 1)
                        level += 1;
                }
            }
            //Print footer
            row = std::accumulate(headers.begin(), headers.end(), string(" "), a_header_footer);
            buffer.push_back(row);
        }
    }
    return buffer;
}

std::string build_csv_row(const std::vector<std::string>& chunks, const std::string& delimiter){
    return std::accumulate(chunks.begin(), chunks.end(), std::string(""),
                           [delimiter](const string &left, const string &right){
                               return left.empty() ? right : left + delimiter + right;
                           });
}

std::string get_root_port_dev(const bool show_root_port, int part_id,  const pcm::iio_stack *stack){
    char tmp[9] = "        ";
    std::string rp_pci;
    for (auto part = stack->parts.begin(); part != stack->parts.end(); part = std::next(part)){
        if (part->part_id == part_id){
            std::snprintf(tmp, sizeof(tmp), "%02x:%02x.%1d", part->root_pci_dev.bdf.busno, part->root_pci_dev.bdf.devno, part->root_pci_dev.bdf.funcno);
            break;
        }
    }
    //cout<<"part_id="<<part_id<<"    dev="<<tmp<<endl;
    rp_pci.append(tmp);
    return rp_pci;
}
std::string get_bus_no(const struct pci p){
    char tmp[9] = "        ";
    std::string rp_pci;
    snprintf(tmp, sizeof(tmp), "%02x:%02x.%1d", p.bdf.busno, p.bdf.devno, p.bdf.funcno);
    //cout<<"    dev="<<tmp<<endl;
    rp_pci.append(tmp);
    return rp_pci;
}

vector<string> build_csv(vector<struct iio_stacks_on_socket>& iios, vector<struct counter>& ctrs, const PCIDB& pciDB){
    vector<string> result;
    vector<string> current_row;
    auto header = combine_stack_name_and_counter_names("Part");
    header.insert(header.begin(), "Name");
    header.insert(header.begin(), "BusNo");
    header.insert(header.begin(), "Socket");
    result.push_back(build_csv_row(header, csv_delimiter));
    std::map<uint32_t,map<uint32_t,struct counter*>> v_sort;
    //re-organize data collection to be row wise
    size_t max_name_width = 0;
    for (std::vector<struct counter>::iterator counter = ctrs.begin(); counter != ctrs.end(); ++counter) {
        v_sort[counter->v_id][counter->h_id] = &(*counter);
        max_name_width = (std::max)(max_name_width, counter->v_event_name.size());
    }

    for (auto socket = iios.cbegin(); socket != iios.cend(); ++socket) {
        for (auto stack = socket->stacks.cbegin(); stack != socket->stacks.cend(); ++stack) {
            const std::string socket_name = "Socket" + std::to_string(socket->socket_id);
            int main_part_id;
            string bus_no;
            for (const auto& part : stack->parts) {
                for (const auto& pci_device : part.child_pci_devs) {
                    bus_no = get_bus_no(pci_device);
                    main_part_id=part.part_id;
                    //cout<<"main_part_id="<<part.part_id<<"   bus_no="<<bus_no<<endl;
                }
            }
            if (!std::count(ONLY.begin(), ONLY.end(), bus_no)) {
                continue;
            }
            std::string stack_name = stack->stack_name;
            stack_name.erase(stack_name.find_last_not_of(' ') + 1);
            const uint32_t stack_id = stack->iio_unit_id;
            int part_id;
            std::map<uint32_t,map<uint32_t,struct counter*>>::const_iterator vunit;
            for (vunit = v_sort.cbegin(), part_id = 0; vunit != v_sort.cend(); ++vunit, ++part_id) {
                map<uint32_t, struct counter*> h_array = vunit->second;
                uint32_t vv_id = vunit->first;
                vector<uint64_t> h_data;
                string v_name = h_array[0]->v_event_name;
                //v_name += string(max_name_width - (v_name.size()), ' ');
                current_row.clear();
                current_row.push_back(socket_name);
                //auto pci_dev = get_root_port_dev(true, part_id, &(*stack));
                //cout<<"part_id="<<part_id<<"   pci_dev="<<pci_dev<<endl;
                current_row.push_back(bus_no);
                current_row.push_back(stack_name);
                current_row.push_back(v_name);
                for (map<uint32_t,struct counter*>::const_iterator hunit = h_array.cbegin(); hunit != h_array.cend(); ++hunit) {
                    uint32_t hh_id = hunit->first;
                    uint64_t raw_data = hunit->second->data[0][socket->socket_id][stack_id][std::pair<h_id,v_id>(hh_id,vv_id)];
                    current_row.push_back(std::to_string(raw_data));  // unit_format(raw_data)
                }
                result.push_back(build_csv_row(current_row, csv_delimiter));
            }
        }
    }
    return result;
}

void display(const vector<string> &buff, std::ostream& stream){
    for (std::vector<string>::const_iterator iunit = buff.begin(); iunit != buff.end(); ++iunit)
        stream << *iunit << "\n";
    stream << std::flush;
}

class IPlatformMapping {
private:
public:
    virtual ~IPlatformMapping() {};
    static IPlatformMapping* getPlatformMapping(int cpu_model);
    virtual bool pciTreeDiscover(std::vector<struct iio_stacks_on_socket>& iios, uint32_t sockets_count) = 0;
};

class IPlatformMapping10Nm: public IPlatformMapping {
private:
public:
    bool getSadIdRootBusMap(uint32_t socket_id, std::map<uint8_t, uint8_t>& sad_id_bus_map);
};

bool IPlatformMapping10Nm::getSadIdRootBusMap(uint32_t socket_id, std::map<uint8_t, uint8_t>& sad_id_bus_map){
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t device = 0; device < 32; device++) {
            for (uint8_t function = 0; function < 8; function++) {
                struct pci pci_dev;
                pci_dev.bdf.busno = (uint8_t)bus;
                pci_dev.bdf.devno = device;
                pci_dev.bdf.funcno = function;
                if (probe_pci(&pci_dev) && (pci_dev.vendor_id == PCM_INTEL_PCI_VENDOR_ID)
                    && (pci_dev.device_id == SNR_ICX_MESH2IIO_MMAP_DID)) {

                    PciHandleType h(0, bus, device, function);
                    std::uint32_t sad_ctrl_cfg;
                    h.read32(SNR_ICX_SAD_CONTROL_CFG_OFFSET, &sad_ctrl_cfg);
                    if (sad_ctrl_cfg == (std::numeric_limits<uint32_t>::max)()) {
                        cerr << "Could not read SAD_CONTROL_CFG" << endl;
                        return false;
                    }

                    if ((sad_ctrl_cfg & 0xf) == socket_id) {
                        uint8_t sid = (sad_ctrl_cfg >> 4) & 0x7;
                        sad_id_bus_map.insert(std::pair<uint8_t, uint8_t>(sid, (uint8_t)bus));
                    }
                }
            }
        }
    }

    if (sad_id_bus_map.empty()) {
        cerr << "Could not find Root Port bus numbers" << endl;
        return false;
    }

    return true;
}

class WhitleyPlatformMapping: public IPlatformMapping10Nm {
private:
    const bool icx_d;
    const std::map<int, int>& sad_to_pmu_id_mapping;
    const std::string * iio_stack_names;
public:
    WhitleyPlatformMapping() :
        icx_d(PCM::getInstance()->getCPUModelFromCPUID() == PCM::ICX_D),
        sad_to_pmu_id_mapping(icx_d ? icx_d_sad_to_pmu_id_mapping : icx_sad_to_pmu_id_mapping),
        iio_stack_names(icx_d ? icx_d_iio_stack_names : icx_iio_stack_names)
    {}
    ~WhitleyPlatformMapping() = default;
    bool pciTreeDiscover(std::vector<struct iio_stacks_on_socket>& iios, uint32_t sockets_count) override;
};

bool WhitleyPlatformMapping::pciTreeDiscover(std::vector<struct iio_stacks_on_socket>& iios, uint32_t sockets_count){
    for (uint32_t socket = 0; socket < sockets_count; socket++) {
        struct iio_stacks_on_socket iio_on_socket;
        iio_on_socket.socket_id = socket;
        std::map<uint8_t, uint8_t> sad_id_bus_map;
        if (!getSadIdRootBusMap(socket, sad_id_bus_map)) return false;
        {
            struct iio_stack stack;
            stack.iio_unit_id = sad_to_pmu_id_mapping.at(ICX_MCP_SAD_ID);
            stack.stack_name = iio_stack_names[stack.iio_unit_id];
            iio_on_socket.stacks.push_back(stack);
        }
        for (auto sad_id_bus_pair = sad_id_bus_map.cbegin(); sad_id_bus_pair != sad_id_bus_map.cend(); ++sad_id_bus_pair) {
            int sad_id = sad_id_bus_pair->first;
            if (sad_to_pmu_id_mapping.find(sad_id) == sad_to_pmu_id_mapping.end()) {
                cerr << "Unknown SAD ID: " << sad_id << endl;
                return false;
            }
            if (sad_id == ICX_MCP_SAD_ID) {
                continue;
            }
            struct iio_stack stack;
            int root_bus = sad_id_bus_pair->second;
            if (sad_id == ICX_CBDMA_DMI_SAD_ID) {
                // There is one DMA Controller on each socket
                stack.iio_unit_id = sad_to_pmu_id_mapping.at(sad_id);
                stack.busno = root_bus;
                stack.stack_name = iio_stack_names[stack.iio_unit_id];

                // PCH is on socket 0 only
                if (socket == 0) {
                    struct iio_bifurcated_part pch_part;
                    struct pci *pci = &pch_part.root_pci_dev;
                    struct bdf *bdf = &pci->bdf;
                    pch_part.part_id = ICX_PCH_PART_ID;
                    bdf->busno = root_bus;
                    bdf->devno = 0x00;
                    bdf->funcno = 0x00;
                    probe_pci(pci);
                    // Probe child devices only under PCH part.
                    for (uint8_t bus = pci->secondary_bus_number; bus <= pci->subordinate_bus_number; bus++) {
                        for (uint8_t device = 0; device < 32; device++) {
                            for (uint8_t function = 0; function < 8; function++) {
                                struct pci child_pci_dev;
                                child_pci_dev.bdf.busno = bus;
                                child_pci_dev.bdf.devno = device;
                                child_pci_dev.bdf.funcno = function;
                                if (probe_pci(&child_pci_dev)) {
                                    pch_part.child_pci_devs.push_back(child_pci_dev);
                                }
                            }
                        }
                    }
                    stack.parts.push_back(pch_part);
                }

                struct iio_bifurcated_part part;
                part.part_id = ICX_CBDMA_PART_ID;
                struct pci *pci = &part.root_pci_dev;
                struct bdf *bdf = &pci->bdf;
                bdf->busno = root_bus;
                bdf->devno = 0x01;
                bdf->funcno = 0x00;
                probe_pci(pci);
                stack.parts.push_back(part);

                iio_on_socket.stacks.push_back(stack);
                continue;
            }
            stack.busno = root_bus;
            stack.iio_unit_id = sad_to_pmu_id_mapping.at(sad_id);
            stack.stack_name = iio_stack_names[stack.iio_unit_id];
            for (int slot = 2; slot < 6; slot++) {
                struct pci pci;
                pci.bdf.busno = root_bus;
                pci.bdf.devno = slot;
                pci.bdf.funcno = 0x00;
                if (!probe_pci(&pci)) {
                    continue;
                }
                struct iio_bifurcated_part part;
                part.part_id = slot - 2;
                part.root_pci_dev = pci;

                for (uint8_t bus = pci.secondary_bus_number; bus <= pci.subordinate_bus_number; bus++) {
                    for (uint8_t device = 0; device < 32; device++) {
                        for (uint8_t function = 0; function < 8; function++) {
                            struct pci child_pci_dev;
                            child_pci_dev.bdf.busno = bus;
                            child_pci_dev.bdf.devno = device;
                            child_pci_dev.bdf.funcno = function;
                            if (probe_pci(&child_pci_dev)) {
                                part.child_pci_devs.push_back(child_pci_dev);
                            }
                        }
                    }
                }
                stack.parts.push_back(part);
            }
            iio_on_socket.stacks.push_back(stack);
        }
        std::sort(iio_on_socket.stacks.begin(), iio_on_socket.stacks.end());
        iios.push_back(iio_on_socket);
    }
    return true;
}

IPlatformMapping* IPlatformMapping::getPlatformMapping(int cpu_model){
    switch (cpu_model) {
    case PCM::SKX:
        //return new PurleyPlatformMapping();
    case PCM::ICX:
        return new WhitleyPlatformMapping();
    case PCM::SNOWRIDGE:
        //return new JacobsvillePlatformMapping();
    default:
        return nullptr;
    }
}

std::string dos2unix(std::string in){
    if (in.length() > 0 && int(in[in.length() - 1]) == 13){
        in.erase(in.length() - 1);
    }
    return in;
}

ccr* get_ccr(PCM* m, uint64_t& ccr){
    switch (m->getCPUModel()){
        case PCM::SKX:
            //return new skx_ccr(ccr);
        case PCM::ICX:
        case PCM::SNOWRIDGE:
            return new icx_ccr(ccr);
        default:
            cerr << "Skylake Server CPU is required for this tool! Program aborted" << endl;
            exit(EXIT_FAILURE);
    }
}

vector<struct counter> load_events(PCM * m, const char* fn){
    vector<struct counter> v;
    struct counter ctr{};
    std::unique_ptr<ccr> pccr(get_ccr(m, ctr.ccr));

    std::ifstream in(fn);
    std::string line, item;

    if (!in.is_open()){
        const auto alt_fn = std::string("/usr/share/pcm/") + fn;
        in.open(alt_fn);
        if (!in.is_open()){
            const auto err_msg = std::string("event file ") + fn + " or " + alt_fn + " is not available. Copy it from PCM build directory.";
            throw std::invalid_argument(err_msg);
        }
    }

    while (std::getline(in, line)) {
        /* Ignore anyline with # */
        //TODO: substring until #, if len == 0, skip, else parse normally
        pccr->set_ccr_value(0);
        if (line.find("#") != std::string::npos)
            continue;
        /* If line does not have any deliminator, we ignore it as well */
        if (line.find("=") == std::string::npos)
            continue;
        std::istringstream iss(line);
        string h_name, v_name;
        while (std::getline(iss, item, ',')) {
            std::string key, value;
            uint64 numValue;
            /* assume the token has the format <key>=<value> */
            key = item.substr(0,item.find("="));
            value = item.substr(item.find("=")+1);
            istringstream iss2(value);
            iss2 >> setbase(0) >> numValue;

            //cout << "Key:" << key << " Value:" << value << " opcodeFieldMap[key]:" << opcodeFieldMap[key] << "\n";
            switch(opcodeFieldMap[key]) {
                case PCM::H_EVENT_NAME:
                    h_name = dos2unix(value);
                    ctr.h_event_name = h_name;
                    if (nameMap.find(h_name) == nameMap.end()) {
                        /* It's a new horizontal event name */
                        uint32_t next_h_id = (uint32_t)nameMap.size();
                        std::pair<h_id,std::map<string,v_id>> nameMap_value(next_h_id, std::map<string,v_id>());
                        nameMap[h_name] = nameMap_value;
                    }
                    ctr.h_id = (uint32_t)nameMap.size() - 1;
                    break;
                case PCM::V_EVENT_NAME:
                    {
                        v_name = dos2unix(value);
                        ctr.v_event_name = v_name;
                        //XXX: If h_name comes after v_name, we'll have a problem.
                        //XXX: It's very weird, I forgot to assign nameMap[h_name] = nameMap_value earlier (:298), but this part still works?
                        std::map<string,v_id> &v_nameMap = nameMap[h_name].second;
                        if (v_nameMap.find(v_name) == v_nameMap.end()) {
                            v_nameMap[v_name] = (unsigned int)v_nameMap.size() - 1;
                        } else {
                            cerr << "Detect duplicated v_name:" << v_name << "\n";
                            in.close();
                            exit(EXIT_FAILURE);
                        }
                        ctr.v_id = (uint32_t)v_nameMap.size() - 1;
                        break;
                    }
                case PCM::COUNTER_INDEX:
                    ctr.idx = (int)numValue;
                    break;
                case PCM::OPCODE:
                    break;
                case PCM::EVENT_SELECT:
                    pccr->set_event_select(numValue);
                    break;
                case PCM::UMASK:
                    pccr->set_umask(numValue);
                    break;
                case PCM::RESET:
                    pccr->set_reset(numValue);
                    break;
                case PCM::EDGE_DET:
                    pccr->set_edge(numValue);
                    break;
                case PCM::IGNORED:
		    break;
                case PCM::OVERFLOW_ENABLE:
                    pccr->set_ov_en(numValue);
                    break;
                case PCM::ENABLE:
                    pccr->set_enable(numValue);
                    break;
                case PCM::INVERT:
                    pccr->set_invert(numValue);
                    break;
                case PCM::THRESH:
                    pccr->set_thresh(numValue);
                    break;
                case PCM::CH_MASK:
                    pccr->set_ch_mask(numValue);
                    break;
                case PCM::FC_MASK:
                    pccr->set_fc_mask(numValue);
                    break;
                //TODO: double type for multiplier. drop divider variable
                case PCM::MULTIPLIER:
                    ctr.multiplier = (int)numValue;
                    break;
                case PCM::DIVIDER:
                    ctr.divider = (int)numValue;
                    break;
                case PCM::INVALID:
                    cerr << "Field in -o file not recognized. The key is: " << key << "\n";
                    in.close();
                    exit(EXIT_FAILURE);
                    break;
            }
        }
        v.push_back(ctr);
        //cout << "Finish parsing: " << line << " size:" << v.size() << "\n";
        cout << line << " " << std::hex << ctr.ccr << std::dec << "\n";
    }
    cout << std::flush;

    in.close();

    return v;
}

result_content get_IIO_Samples(PCM *m, const std::vector<struct iio_stacks_on_socket>& iios, struct counter ctr, uint32_t delay_ms){
    IIOCounterState *before, *after;
    uint64 rawEvents[4] = {0};
    std::unique_ptr<ccr> pccr(get_ccr(m, ctr.ccr));
    rawEvents[ctr.idx] = pccr->get_ccr_value();
    const int stacks_count = (int)m->getMaxNumOfIIOStacks();
    before = new IIOCounterState[iios.size() * stacks_count];
    after = new IIOCounterState[iios.size() * stacks_count];

    m->programIIOCounters(rawEvents);
    for (auto socket = iios.cbegin(); socket != iios.cend(); ++socket) {
        for (auto stack = socket->stacks.cbegin(); stack != socket->stacks.cend(); ++stack) {
            auto iio_unit_id = stack->iio_unit_id;
            uint32_t idx = (uint32_t)stacks_count * socket->socket_id + iio_unit_id;
            before[idx] = m->getIIOCounterState(socket->socket_id, iio_unit_id, ctr.idx);
        }
    }
    MySleepMs(delay_ms);
    for (auto socket = iios.cbegin(); socket != iios.cend(); ++socket) {
        for (auto stack = socket->stacks.cbegin(); stack != socket->stacks.cend(); ++stack) {
            auto iio_unit_id = stack->iio_unit_id;
            uint32_t idx = (uint32_t)stacks_count * socket->socket_id + iio_unit_id;
            after[idx] = m->getIIOCounterState(socket->socket_id, iio_unit_id, ctr.idx);
            uint64_t raw_result = getNumberOfEvents(before[idx], after[idx]);
            uint64_t trans_result = uint64_t (raw_result * ctr.multiplier / (double) ctr.divider * (1000 / (double) delay_ms));
            results[socket->socket_id][iio_unit_id][std::pair<h_id,v_id>(ctr.h_id,ctr.v_id)] = trans_result;
        }
    }
    delete[] before;
    delete[] after;
    return results;
}

void collect_data(PCM *m, const double delay, vector<struct iio_stacks_on_socket>& iios, vector<struct counter>& ctrs){
    const uint32_t delay_ms = uint32_t(delay * 1000 / ctrs.size());
    for (auto counter = ctrs.begin(); counter != ctrs.end(); ++counter) {
        counter->data.clear();
        result_content sample = get_IIO_Samples(m, iios, *counter, delay_ms);
        counter->data.push_back(sample);
    }
}

void print_PCIeMapping(const std::vector<struct iio_stacks_on_socket>& iios, const PCIDB & pciDB){
    for (auto it = iios.begin(); it != iios.end(); ++it) {
        printf("Socket %d\n", (*it).socket_id);
        for (int stack = 0; stack < 6; stack++) {
            for (auto & stack : it->stacks) {
                printf("\t%s root bus: 0x%x", stack.stack_name.c_str(), stack.busno);
		printf("\tflipped: %s\n", stack.flipped ? "true" : "false");
                for (auto& part : stack.parts) {
                    vector<struct pci> pp = part.child_pci_devs;
                    uint8_t level = 1;
                    for (std::vector<struct pci>::const_iterator iunit = pp.begin(); iunit != pp.end(); ++iunit)
                    {
                        uint64_t header_width = 100;
                        string row = build_pci_header(pciDB, (uint32_t)header_width, *iunit, -1, level);
                        printf("\t\t%s\n", row.c_str());
                        if (iunit->header_type == 1)
                            level += 1;
                    }
                }
            }
        }
    }
}

void split_only(string ids){
    stringstream ss(ids);
    string str;
    while (getline(ss, str, ',')) {
        ONLY.push_back(str);
    }
    cout<<"ONLY="<<ONLY.size()<<endl;
}

int main(int argc, char** argv) {
    cxxopts::Options options("pcie", "pcie performance monitor tool");
    options.add_options()
        ("g,debug",   "Enable debug info",    cxxopts::value<bool>()->default_value("false"))
        ("v,version", "Version output",       cxxopts::value<bool>()->default_value("false"))
        ("o,output",  "Write to csv file",    cxxopts::value<string>()->default_value(""))
        ("s,delay",   "Seconds/update",       cxxopts::value<float>()->default_value("2.0"))
        ("l,only",    "Show only pcie list",  cxxopts::value<string>()->default_value(""))
        ("h,help",    "Print usage")
        //("n,duration","Duration",           cxxopts::value<int>()->default_value("60"))
    ;
    auto result = options.parse(argc, argv);
    if (result.count("help")){
      std::cout << options.help() << std::endl;
      exit(0);
    }
    if (result.count("version")){
      std::cout << "Intel pcie performance monitor tool (ICX)\n" << "version: 0.0.1" << std::endl;
      exit(0);
    }
    delay=result["delay"].as<float>();
    DEBUG = result["debug"].as<bool>();
    string s_only = result["only"].as<string>();
    split_only(s_only);
    OUT_FILE=result["output"].as<string>();

    vector<struct counter> counters;
    PCIDB pciDB;
    load_PCIDB(pciDB);
    bool csv = false;
    MainLoop mainLoop;
    PCM * m = PCM::getInstance();

    string ev_file_name;
    if (m->IIOEventsAvailable()){
        ev_file_name = "opCode-" + std::to_string(m->getCPUModel()) + ".txt";
    }else{
        cerr << "This CPU is not supported by PCM IIO tool! Program aborted\n";
        exit(EXIT_FAILURE);
    }
    opcodeFieldMap["opcode"] = PCM::OPCODE;
    opcodeFieldMap["ev_sel"] = PCM::EVENT_SELECT;
    opcodeFieldMap["umask"] = PCM::UMASK;
    opcodeFieldMap["reset"] = PCM::RESET;
    opcodeFieldMap["edge_det"] = PCM::EDGE_DET;
    opcodeFieldMap["ignored"] = PCM::IGNORED;
    opcodeFieldMap["overflow_enable"] = PCM::OVERFLOW_ENABLE;
    opcodeFieldMap["en"] = PCM::ENABLE;
    opcodeFieldMap["invert"] = PCM::INVERT;
    opcodeFieldMap["thresh"] = PCM::THRESH;
    opcodeFieldMap["ch_mask"] = PCM::CH_MASK;
    opcodeFieldMap["fc_mask"] = PCM::FC_MASK;
    opcodeFieldMap["hname"] =PCM::H_EVENT_NAME;
    opcodeFieldMap["vname"] =PCM::V_EVENT_NAME;
    opcodeFieldMap["multiplier"] = PCM::MULTIPLIER;
    opcodeFieldMap["divider"] = PCM::DIVIDER;
    opcodeFieldMap["ctr"] = PCM::COUNTER_INDEX;

    counters = load_events(m, ev_file_name.c_str());

    auto mapping = IPlatformMapping::getPlatformMapping(m->getCPUModel());
    if (!mapping) {
        cerr << "Failed to discover pci tree: unknown platform" << endl;
        exit(EXIT_FAILURE);
    }

    std::vector<struct iio_stacks_on_socket> iios;
    if (!mapping->pciTreeDiscover(iios, m->getNumSockets())) {
        exit(EXIT_FAILURE);
    }

    if (DEBUG){
        print_cpu_details();
        print_nameMap();
        print_PCIeMapping(iios, pciDB);
    }
    std::fstream file_stream;
    if (OUT_FILE.size()>0) {
        file_stream.open(OUT_FILE.c_str(), std::ios_base::out);
        OUT = &file_stream;
    }

    mainLoop([&](){
        collect_data(m, delay, iios, counters);
        //vector<string> display_buffer = csv ? build_csv(iios, counters, true) : build_display(iios, counters, pciDB);
        vector<string> display_buffer = build_csv(iios, counters, pciDB);
        display(display_buffer, *OUT);
        return true;
    });

    file_stream.close();
    exit(EXIT_SUCCESS);
}

