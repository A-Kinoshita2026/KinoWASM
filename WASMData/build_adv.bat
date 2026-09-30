@em++ -Wl,--no-entry -Wl,--allow-undefined -nostdlib -std=c++20 -O2 --target=wasm32 -o start.wasm system/Main.cpp Adv.cpp
