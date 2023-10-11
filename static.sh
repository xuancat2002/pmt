#cd /data/tools/pmt/
#yum install glibc-static libstdc++-static
rm -rf pcie mem 
g++  main.cpp -o mem  -std=c++11 -Ipcm/src/ -Llib/ -lpcm -lpthread -ldl -static  # libpcm.a
g++  pcie.cpp -o pcie -std=c++11 -Ipcm/src/ -Llib/ -lpcm -lpthread -ldl -static 


#ids=`lspci|grep acc|awk '{print $1}'| tr '\n' ','`
#./pcie --only=$ids
