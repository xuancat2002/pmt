#yum install glibc-static libstdc++-static
g++  pcie.cpp -o pcie  \
  -std=c++11 -Ipcm/src/ -Llib/ -lpcm -lpthread -ldl -static 
