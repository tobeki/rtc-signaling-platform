// Linux-only compatibility forwarding header. No business logic.
//
// Forwards <jdbc/cppconn/exception.h> to Ubuntu's <cppconn/exception.h>.
// See jdbc/mysql_driver.h for the rationale.
#pragma once

#include <cppconn/exception.h>
