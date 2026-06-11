#############################################################################
# Copyright (c) 2025 Martin Erich Jobst
#
# This program and the accompanying materials are made available under the
# terms of the Eclipse Public License 2.0 which is available at
# http://www.eclipse.org/legal/epl-2.0.
#
# SPDX-License-Identifier: EPL-2.0
#
# Contributors:
#    Martin Erich Jobst
#      - initial API and implementation and/or initial documentation
#############################################################################

add_library(forte-core)

## BEGIN - Enforcement definition
option(FORTE_TRACE_EVENTS "FORTE will log the events received at and sent from function blocks" OFF)
if(FORTE_TRACE_EVENTS)
  target_compile_definitions(forte-core PUBLIC FORTE_TRACE_EVENTS)
endif()

option(FORTE_EET_MONITORING "Enable EET execution time measurement" OFF)
option(FORTE_EET_EVALUATION
  "Enable evaluation infrastructure (CSV export, signal handlers)" OFF)
option(FORTE_FET_ENFORCEMENT "Enable FET deadline enforcement" OFF)


if(FORTE_FET_ENFORCEMENT AND NOT FORTE_EET_MONITORING)
  message(FATAL_ERROR
    "FORTE_FET_ENFORCEMENT requires FORTE_EET_MONITORING=ON. "
    "FET needs EET to derive and register deadlines.")
endif()

if(FORTE_EET_EVALUATION AND NOT FORTE_EET_MONITORING)
  message(FATAL_ERROR
    "FORTE_EET_EVALUATION requires FORTE_EET_MONITORING=ON. "
    "Evaluation infrastructure requires measurement to be active.")
endif()

if(FORTE_EET_MONITORING)
  target_compile_definitions(forte-core PUBLIC FORTE_EET_MONITORING)
endif()

if(FORTE_EET_EVALUATION)
  target_compile_definitions(forte PRIVATE FORTE_EET_EVALUATION)
endif()

if(FORTE_FET_ENFORCEMENT)
  target_compile_definitions(forte-core PUBLIC FORTE_FET_ENFORCEMENT)
endif()

## END - Enforcement definition

target_compile_features(forte-core PUBLIC cxx_std_20)
target_link_libraries(forte PUBLIC $<IF:$<BOOL:${BUILD_SHARED_LIBS}>,forte-core,$<LINK_LIBRARY:WHOLE_ARCHIVE,forte-core>>)

add_subdirectory(src)
add_subdirectory(include)
add_subdirectory(arch)

install(TARGETS forte-core EXPORT forte-export FILE_SET HEADERS)
