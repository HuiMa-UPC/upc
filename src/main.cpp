#include "platform.hpp"
#include "ui_shell.hpp"
#include "job_status.hpp"

#include <cstdio>

int main()
{
    std::fprintf(stderr, "Rongsu UI shell start\n");

    if(platform_init() != 0) {
        return 1;
    }

    job_status_reset_mcu_position();

    ui_shell_init();

    platform_run_loop();
    return 0;
}
