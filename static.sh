cd /data/pmt/
g++  main.cpp -o pmt  \
  -Ipcm/src/ -Lbuild/lib/ -lpcm -lpthread -ldl libpcm.a -static
#./pmt_static 2>/dev/null
