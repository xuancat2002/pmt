
cd /data/pmt/
g++ -Ipcm/src/ -Lbuild/lib/ -Wl,-rpath=build/lib/ -lpcm -pthread -ldl main.cpp -o pmt
#./pmt 2>/dev/null
