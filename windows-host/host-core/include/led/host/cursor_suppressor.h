#pragma once

#include "led/common/status.h"

namespace led::host {

class CursorSuppressor {
public:
    Status hideSystemCursor();
    Status restoreSystemCursor();
    ~CursorSuppressor();

private:
    bool hidden_{false};
};

}  // namespace led::host
