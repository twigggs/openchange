/*
   Schema migration util procedures calling Python code

   OpenChange Project

   Copyright (C) Enrique J. Hernández 2015

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __SCHEMA_MIGRATION_H__
#define __SCHEMA_MIGRATION_H__

#include <talloc.h>

int migrate_openchangedb_schema(const char *);
int migrate_indexing_schema(const char *);
int migrate_named_properties_schema(const char *);

#endif /* __SCHEMA_MIGRATION_H__ */
