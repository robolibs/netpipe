#pragma once

#include <datapod/datapod.hpp>
#include <echo/echo.hpp>

namespace netpipe {

    dp::Result<void> test_func() {
        echo("Hello World!");
        return dp::result::ok();
    }

} // namespace netpipe
