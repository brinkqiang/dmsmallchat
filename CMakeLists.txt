cmake_minimum_required(VERSION 3.8)

PROJECT(dmconfigserver)
INCLUDE(cmake/ModuleImport.cmake)
INCLUDE(cmake/ModuleCompileOptions.cmake)
ModuleSetCompileOptions()

SET(DMASIO_VERSION_MAJOR "1")
SET(DMASIO_VERSION_MINOR "0")
SET(DMASIO_VERSION_PATCH "1")
SET(DMASIO_VERSION "${DMASIO_VERSION_MAJOR}.${DMASIO_VERSION_MINOR}.${DMASIO_VERSION_PATCH}")

MESSAGE(STATUS "VERSION: ${DMASIO_VERSION}")

LibImport("chatlib" "src/chatlib")
ExeImport("example" "chatlib")
