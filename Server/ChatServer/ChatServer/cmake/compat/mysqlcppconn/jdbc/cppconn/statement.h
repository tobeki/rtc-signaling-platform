// Linux-only compatibility forwarding header. No business logic.
//
// Forwards <jdbc/cppconn/statement.h> to Ubuntu's <cppconn/statement.h>.
// See jdbc/mysql_driver.h for the rationale.
#pragma once

#include <cppconn/statement.h>
