// Linux-only compatibility forwarding header. No business logic.
//
// The application source (MysqlDao.h) includes <jdbc/mysql_driver.h>, which is
// the header layout of the Windows MySQL Connector/C++ tree. Ubuntu's
// libmysqlcppconn-dev installs the same header with a flat layout at
// <mysql_driver.h>. This forwarding header keeps the application source
// unchanged on both platforms.
//
// CMakeLists.txt adds this compatibility include root with BEFORE, so it is
// searched ahead of the normal system include path.
#pragma once

#include <mysql_driver.h>
