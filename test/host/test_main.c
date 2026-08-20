#include "nuwax_test.h"

/* Shared assertion counters (declared extern in nuwax_test.h). */
int nuwax_test_failures = 0;
int nuwax_test_checks = 0;

/* Per-suite entry points. */
void nuwax_test_style(void);
void nuwax_test_data(void);
void nuwax_test_wifi(void);
void nuwax_test_status(void);
void nuwax_test_wheel(void);
void nuwax_test_surface_style(void);

int main(void) {
    printf("== nuwax focus-menu host unit tests ==\n");

    printf("[style]\n");        nuwax_test_style();
    printf("[data]\n");         nuwax_test_data();
    printf("[wifi]\n");         nuwax_test_wifi();
    printf("[status]\n");       nuwax_test_status();
    printf("[wheel]\n");        nuwax_test_wheel();
    printf("[surface_style]\n"); nuwax_test_surface_style();

    printf("\n== %d checks, %d failure(s) ==\n", nuwax_test_checks, nuwax_test_failures);
    return nuwax_test_failures == 0 ? 0 : 1;
}
