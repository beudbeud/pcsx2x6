add_library(pcsx2_libretro MODULE)

set_target_properties(pcsx2_libretro PROPERTIES
	PREFIX ""
	OUTPUT_NAME "pcsx2_libretro"
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
	LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if (PACKAGE_MODE)
	install(TARGETS pcsx2_libretro DESTINATION ${CMAKE_INSTALL_LIBDIR})
else()
	install(TARGETS pcsx2_libretro DESTINATION ${CMAKE_SOURCE_DIR}/bin)
endif()

target_sources(pcsx2_libretro PRIVATE
	LibretroCore.cpp
	libretro.h
)

target_include_directories(pcsx2_libretro PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${CMAKE_BINARY_DIR}/common/include"
	"${CMAKE_SOURCE_DIR}/pcsx2"
	"${CMAKE_SOURCE_DIR}"
)

target_link_libraries(pcsx2_libretro PRIVATE
	PCSX2_FLAGS
	PCSX2
)

# Fail at link time on undefined symbols rather than at runtime load.
# Full RELRO (relro + now): the GOT becomes read-only after loading, so a stray
# store from the recompilers corrupts nothing silently — it faults at the writer.
if(UNIX AND NOT APPLE)
	target_link_options(pcsx2_libretro PRIVATE "-Wl,--no-undefined" "-Wl,-z,noexecstack" "-Wl,-z,relro" "-Wl,-z,now")
elseif(APPLE)
	target_link_options(pcsx2_libretro PRIVATE "-Wl,-w")
endif()
