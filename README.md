# quickfix-benchmark

- You must compile quickfix statically.

- Compile the benchmark:

```g++ -g  benchmark.cpp -O3 -flto  -o fix_benchmark  -std=c++17  -lpthread -I../quickfix/include/  ../quickfix/lib/libquickfix.a  -march=native```

- Run it:

```
./fix_benchmark 
Messages: 9000
Min: 88 µs
Max: 180 µs
Avg: 149.58 µs
P50: 150 µs
P99: 180 µs
```
