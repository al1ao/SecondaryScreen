#pragma once
#include "net/pipeline_session.h"
namespace pds {
bool ParseSessionOptions(int argc, char** argv, SessionOptions& options);
}
