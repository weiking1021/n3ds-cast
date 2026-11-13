#pragma once
#include <3ds.h>
#include "protocol.h"
Result native_request(Handle client, unsigned command, const NativeStartConfig *start, NativeStatus *status);
Result native_connect(Handle *client, NativeStatus *status);
Result native_launch(Handle *client, NativeStatus *status, char *error, size_t error_capacity);
