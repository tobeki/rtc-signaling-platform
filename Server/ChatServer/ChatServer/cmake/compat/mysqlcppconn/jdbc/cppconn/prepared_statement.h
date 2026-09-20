// Linux-only compatibility forwarding header. No business logic.
//
// Forwards the Windows Connector/C++ include path
// <jdbc/cppconn/prepared_statement.h> to Ubuntu's layout
// <cppconn/prepared_statement.h>. See jdbc/mysql_driver.h for the rationale.
#pragma once

#include <cppconn/prepared_statement.h>
