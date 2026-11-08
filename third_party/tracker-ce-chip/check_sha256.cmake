# SPDX-License-Identifier: GPL-3.0-or-later
# ctest cechip_oracle_sha: the two vendored files are byte for byte what PROVENANCE.md says (tracker-ce@7fd6c2c).
# Usage: cmake -DDIR=<this directory> -DSRC_SHA256=<hash> -DLICENSE_SHA256=<hash> -P check_sha256.cmake
foreach(pair "ChiptuneSynthVoice.cpp;${SRC_SHA256}" "LICENSE;${LICENSE_SHA256}")
  list(GET pair 0 name)
  list(GET pair 1 expected)
  file(SHA256 "${DIR}/${name}" actual)
  if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "${name} is not the vendored original: SHA-256 ${actual}, expected ${expected}")
  endif()
  message(STATUS "${name}: SHA-256 ${actual}")
endforeach()
