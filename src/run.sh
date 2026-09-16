if [ -n "$1" ]; then
    num_threads="$1"
else
    num_threads=$(nproc --all)
fi

if [ -n "$2" ]; then
    num_cores="$2"
else
    num_cores=$(grep ^cpu\\scores /proc/cpuinfo | uniq |  awk '{print $4}')
fi

rm -f main
rm -f result.txt
g++ -o main main.cpp -O3 -std=c++23 -march=native -m64 -lpthread -DN_THREADS_PARAM=$num_threads -DN_CORES_PARAM=$num_cores -g
time ./main measurements.txt