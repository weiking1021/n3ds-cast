#pragma once
// Reads a 32-bit hardware register (an address in the ARM11 I/O range). Kept in its own file so that the code
// that decides what to do with the values can be tested against fake register contents.
unsigned native_hw_read(unsigned address);
