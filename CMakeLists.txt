# Copyright (c) 2026, FASTMEM developers
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335 USA

SET(FASTMEM_SOURCES
  fm_create.cc
  fm_data.cc
  fm_hash.cc
  ha_fastmem.cc)

MYSQL_ADD_PLUGIN(fastmem ${FASTMEM_SOURCES} STORAGE_ENGINE RECOMPILE_FOR_EMBEDDED)