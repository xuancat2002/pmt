#cd /data/tools/pmt/
#yum install glibc-static libstdc++-static
#g++  main.cpp -o pmt -std=c++11 -Ipcm/src/ -Llib/ -lpcm -lpthread -ldl -static  # libpcm.a
g++  pcie.cpp -o pcie -std=c++11 -Ipcm/src/ -Llib/ -lpcm -lpthread -ldl -static 
