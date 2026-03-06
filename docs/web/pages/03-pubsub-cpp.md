## Pub/Sub in C++

### Example flow

```bash
./build/rosemaster &
./build/test_cpp_subscriber cpp_chatter 10
./build/test_cpp_publisher cpp_chatter 10 100
```

### C API from C++

`ros3.h` is C-compatible in C++ mode. Use the same pub/sub API from C++ translation units.

### Typical compile line

```bash
c++ -O2 -std=c++17 -I/usr/local/include app.cpp -L/usr/local/lib -lros3 -lmirage -lquicksand -o app
```
