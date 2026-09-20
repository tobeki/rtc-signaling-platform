// Linux-only compatibility forwarding header. No business logic.
//
// Forwards <jdbc/cppconn/resultset.h> to Ubuntu's <cppconn/resultset.h>.
// See jdbc/mysql_driver.h for the rationale.
#pragma once

#include <cppconn/resultset.h>
