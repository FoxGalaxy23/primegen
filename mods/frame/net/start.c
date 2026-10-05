#include "net.h"
#include "nal.h"
#include <stdlib.h>

char* network_http_get(const char* url) {
    return nal_http_get(url);
}
