// Example client of the Wetland C API (also a link check for the library).
#include <cstdio>
#include "Wetland_api.h"

int main()
{
    Wetland_handle* h = Wetland_create();
    Wetland_initialize(h);
    const int ok = Wetland_run_to(h, Wetland_simulation_end());
    std::printf("t=%g %s\n", Wetland_time(h), ok ? "OK" : "FAILED");
    for (int i = 0; i < Wetland_n_states(); ++i)
        std::printf("  %s = %.10g\n", Wetland_state_name(i), Wetland_state(h, i));
    Wetland_destroy(h);
    return ok ? 0 : 2;
}
