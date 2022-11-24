cd /data/tools/pmt/
#yum install glibc-static libstdc++-static
g++  main.cpp -o pmt  \
  -std=c++11 -Ipcm/src/ -Llib/ -lpcm -lpthread -ldl -static  # libpcm.a
#./pmt_static 2>/dev/null
