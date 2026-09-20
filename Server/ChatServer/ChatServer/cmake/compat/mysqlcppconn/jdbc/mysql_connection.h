// Linux-only compatibility forwarding header. No business logic.
//
// Forwards the Windows Connector/C++ include path <jdbc/mysql_connection.h> to
// Ubuntu's flat layout <mysql_connection.h>. See jdbc/mysql_driver.h for the
// full rationale.
#pragma once

#include <mysql_connection.h>
