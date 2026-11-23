# SceShell plugin for background downloads, see main.c.
# Built with its own flags: it runs inside SceShell and must not pull in newlib.

add_executable(switchfin_bgdl main.c)
target_include_directories(switchfin_bgdl PRIVATE ${CMAKE_SOURCE_DIR}/app/include)
# gnu17: taiHEN's TAI_CONTINUE calls through int (*)(), which C23 (GCC 15's
# default) reads as "no arguments".
target_compile_options(switchfin_bgdl PRIVATE -std=gnu17 -Wall -O2 -fno-builtin)
target_link_options(switchfin_bgdl PRIVATE -Wl,-q -nostdlib)
target_link_libraries(switchfin_bgdl
    taihen_stub
    SceLibKernel_stub
    SceIofilemgr_stub
    SceKernelModulemgr_stub
    SceKernelThreadMgr_stub
    gcc
)
vita_create_self(switchfin_bgdl.suprx switchfin_bgdl CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/exports.yml UNSAFE)
