
cd /data/pmt/
g++ -Ipcm/src/ -Lbuild/lib/ -lpcm -Wl,-rpath=build/lib/ -DPCM_DYNAMIC_LIB -pthread -ldl main.cpp -o pmt
./pmt 2>/dev/null
