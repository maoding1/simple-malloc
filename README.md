# simple-malloc
a simple implementation of glibc's malloc

## introductijon
inspired from [njuos 2025 M5: mymalloc](https://jyywiki.cn/OS/2025/labs/M5.md);

It hasn't undergone enough testing yet, and there may still exists many bugs.It is still far from being ready for production use and  can only be used for learning purposes.

Testing(mimalloc-test-stress) has shown that mimalloc's performance is roughly equivalent to that of glibc's malloc and jemalloc (however, its usability and robustness are far inferior to those two).


| Key performance indicators | simple-malloc | glibc malloc | jemalloc |
|---|---|---|---|
| Elapsed Time(s) | 1.398 | 1.388 | 1.419 |
| RSS(Resident Set Size) | 398.2MiB | 435.2MiB | 341.1MiB |
| User CPU Time(s) | 2.992 | 2.874 | 2.968 |

## test steps

### get and compile mimalloc(for benchmark)

```bash
# 1. clone mimalloc 仓库
git clone https://github.com/microsoft/mimalloc.git
cd mimalloc

# 2. compile benchmark
mkdir -p out/release
cd out/release
cmake ../..
make -j mimalloc-test-stress
```

### prepare jemalloc
``` bash
sudo apt-get update
sudo apt-get install libjemalloc-dev
```

### start test
glibc:
```bash
./mimalloc-test-stress 
```

simple-malloc:
```bash
LD_PRELOAD=/path/to/libmyalloc.so ./mimalloc-test-stress
```

jemalloc:
```bash
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libjemalloc.so.2 ./mimalloc-test-stress  
```