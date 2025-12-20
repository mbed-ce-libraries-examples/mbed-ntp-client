//
// Config header for coreSNTP that forwards its log messages to Mbed Trace
//

#pragma once

#include "mbed_trace.h"

#define TRACE_GROUP "coreSNTP"

#define PrintfError( ... )         tr_error(__VA_ARGS__ )
#define PrintfWarn( ... )          tr_warn(__VA_ARGS__ )
#define PrintfInfo( ... )          tr_info(__VA_ARGS__ )
#define PrintfDebug( ... )         tr_debug(__VA_ARGS__ )