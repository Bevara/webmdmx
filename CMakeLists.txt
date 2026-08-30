cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(WEBMDMX_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/dmx_webm.c
        ${CMAKE_CURRENT_SOURCE_DIR}/nestegg.c
)

SET(WEBMDMX_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(webmdmx
        "${WEBMDMX_SRC}"
        ""
        []
        ""
        "${WEBMDMX_INC}"
        ""
        "1")
