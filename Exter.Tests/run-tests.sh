#!/bin/sh
# Tes transport EXTER tanpa Windows (Linux/macOS/WSL). Butuh g++.
set -e
cd "$(dirname "$0")"
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -Wall -Wextra -I../Exter.Shared TransportTests.cpp -pthread -o /tmp/exter_transport_tests
/tmp/exter_transport_tests
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -Wall -Wextra -I../Exter.AudioEngine DspTests.cpp ../Exter.AudioEngine/RadioEffect.cpp -o /tmp/exter_dsp_tests
/tmp/exter_dsp_tests
# Logika modul kernel ExterVmic.cpp terhadap shim API kernel (-fshort-wchar meniru wchar_t 2 byte Windows)
g++ -std=c++17 -fshort-wchar -O1 -g -fsanitize=address,undefined -Wall -Wextra -Wno-unknown-pragmas -pthread -Ikernel-shim \
    -I../Exter.Driver/sysvad/EndpointsCommon -I../Exter.Shared DriverModuleTests.cpp -o /tmp/exter_driver_tests
/tmp/exter_driver_tests
