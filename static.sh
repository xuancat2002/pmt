cd /data/tools/pmt/
#yum install glibc-static libstdc++-static
# -std=c++11
g++  main.cpp -o pmt  \
  -std=c++11 -Ipcm/src/ -Lbuild/lib/ -lpcm -lpthread -ldl libpcm.a -static
#./pmt_static 2>/dev/null
