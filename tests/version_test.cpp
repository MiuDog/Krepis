#include "krepis/version.hpp"

#include <cstdio>

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

}  // namespace

int main() {
    expect(krepis::version_string() == "0.0.1", "version_string matches 0.0.1");
    expect(krepis::version_major == 0, "version_major is 0");
    expect(krepis::version_minor == 0, "version_minor is 0");
    expect(krepis::version_patch == 1, "version_patch is 1");

    if (failures == 0) {
        std::printf("krepis.version: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
