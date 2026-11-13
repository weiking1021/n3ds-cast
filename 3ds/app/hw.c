#include "hw.h"

unsigned native_hw_read(unsigned address) {
    return *(volatile const unsigned *)address;
}
